/// The OpenFX build of Old Cathode, for DaVinci Resolve, Vegas, Nuke, Natron
/// and other OFX hosts.
///
/// The signal path is the same route: encode to composite, damage the wire,
/// decode it back, paint it on curved glass. The standards table
/// (Standards.cpp) is linked straight from source; the five GPU stages of
/// source/shaders/ are mirrored here on the CPU, constant for constant. When
/// editing a stage's GLSL, edit the matching function here.
///
/// Two OFX-specific points:
///
/// - **FrameIndex is the timeline frame.** FFGL counts rendered frames from
///   init; OFX hands the frame number directly, which is better — the
///   subcarrier's frame-to-frame phase walk is deterministic against the
///   edit, so a frame renders identically however the host reaches it.
/// - **The phosphor is reconstructed, not carried.** Emissive decay is
///   max(current, history * decay) per channel, so frame N's trail is the
///   max over previous frames' signals weighted by decay^k — a window the
///   decay rate bounds. Previous frames arrive through OFX temporal clip
///   access; very high Persistence is truncated at kMaxHistory frames.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Standards.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.oldcathode";
constexpr const char* kPluginName       = "Old Cathode";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"An analogue television signal path: the picture is encoded onto a "
	"colour subcarrier, the composite is damaged, and a decoder that cannot "
	"tell luminance from chrominance takes it apart again. Dot crawl, "
	"cross-colour, chroma smear and coloured snow are not drawn — they are "
	"consequences, correlated the way the real ones are. Then a CRT: beam, "
	"phosphor mask, curvature, halation, all in the tube's own "
	"coordinates.\n\n"
	"https://stoatworks-labs.com";

/// The most previous frames the phosphor reconstruction will fetch.
constexpr int kMaxHistory = 12;

constexpr float PI  = 3.14159265359f;
constexpr float TAU = 6.28318530718f;
constexpr int TAPS  = 8;

//---------------------------------------------------------------------------
// Small image planes.
//---------------------------------------------------------------------------
struct Plane4
{
	int w = 0, h = 0;
	std::vector<float> v;

	void resize( int width, int height )
	{
		w = width;
		h = height;
		v.assign( size_t( w ) * h * 4, 0.0f );
	}
	const float* at( int x, int y ) const
	{
		x = std::clamp( x, 0, w - 1 );
		y = std::clamp( y, 0, h - 1 );
		return &v[ ( size_t( y ) * w + x ) * 4 ];
	}
	float* row( int y ) { return &v[ size_t( y ) * w * 4 ]; }
};

/// GL_LINEAR with clamp-to-edge, uv in 0..1, texel centres at half-texel.
void bilinear4( const Plane4& p, float u, float v, float out[ 4 ] )
{
	const float fx = u * p.w - 0.5f;
	const float fy = v * p.h - 0.5f;
	const int x0   = int( std::floor( fx ) );
	const int y0   = int( std::floor( fy ) );
	const float ax = fx - x0;
	const float ay = fy - y0;

	const float* p00 = p.at( x0, y0 );
	const float* p10 = p.at( x0 + 1, y0 );
	const float* p01 = p.at( x0, y0 + 1 );
	const float* p11 = p.at( x0 + 1, y0 + 1 );
	for( int k = 0; k < 4; ++k )
	{
		const float top    = p00[ k ] + ( p10[ k ] - p00[ k ] ) * ax;
		const float bottom = p01[ k ] + ( p11[ k ] - p01[ k ] ) * ax;
		out[ k ]           = top + ( bottom - top ) * ay;
	}
}

void parallelRows( int height, const std::function<void( int, int )>& body )
{
	const int workers = std::max( 1u, std::thread::hardware_concurrency() );
	const int chunk   = std::max( 1, ( height + workers - 1 ) / workers );
	std::vector<std::thread> pool;
	for( int y0 = 0; y0 < height; y0 += chunk )
	{
		const int y1 = std::min( height, y0 + chunk );
		pool.emplace_back( [ =, &body ] { body( y0, y1 ); } );
	}
	for( std::thread& t : pool )
		t.join();
}

inline float smoothstepf( float lo, float hi, float x )
{
	const float t = std::clamp( ( x - lo ) / ( hi - lo ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

inline float fractf( float x )
{
	return x - std::floor( x );
}

inline float lerpf( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

//---------------------------------------------------------------------------
// Mirrors of the signal stage's building blocks (source/shaders/Signal.cpp).
//---------------------------------------------------------------------------
struct Vec3
{
	float x = 0, y = 0, z = 0;
};

inline Vec3 rgbToYuv( float r, float g, float b )
{
	return { 0.299f * r + 0.587f * g + 0.114f * b,
			 -0.14713f * r - 0.28886f * g + 0.436f * b,
			 0.615f * r - 0.51499f * g - 0.10001f * b };
}

inline void yuvToRgb( const Vec3& c, float out[ 3 ] )
{
	out[ 0 ] = c.x + 1.13983f * c.z;
	out[ 1 ] = c.x - 0.39465f * c.y - 0.58060f * c.z;
	out[ 2 ] = c.x + 2.03211f * c.y;
}

inline uint32_t hashU( uint32_t x, uint32_t y, uint32_t z )
{
	uint32_t h = x * 374761393u + y * 668265263u + z * 2246822519u;
	h          = ( h ^ ( h >> 13u ) ) * 1274126177u;
	return h ^ ( h >> 16u );
}

inline float rnd( float a, float b, float c )
{
	const uint32_t k0 = uint32_t( int32_t( std::floor( a ) ) );
	const uint32_t k1 = uint32_t( int32_t( std::floor( b ) ) );
	const uint32_t k2 = uint32_t( int32_t( std::floor( c ) ) );
	return float( hashU( k0, k1, k2 ) & 0x00FFFFFFu ) / 16777216.0f;
}

inline float windowedSinc( float n, float cutoff )
{
	const float x = 2.0f * cutoff * n;
	const float s = std::abs( x ) < 1e-6f ? 1.0f : std::sin( PI * x ) / ( PI * x );
	const float w = std::max( 0.5f + 0.5f * std::cos( PI * n / float( TAPS + 1 ) ), 0.0f );
	return 2.0f * cutoff * s * w;
}

/// Everything one render of the signal stage needs.
struct SignalSettings
{
	int signalW = 754, signalH = 486;
	float samplePhaseStep = 0, linePhaseStep = 0, framePhaseStep = 0;
	bool pal = false;

	float lumaCutoff = 0.2f, chromaCutoff = 0.05f;
	float chromaDelay = 0.0f;
	float notchAmount = 0.4f;
	float saturation  = 1.0f;
	float phaseError  = 0.0f;

	float noise = 0, dropouts = 0, ghostAmount = 0, ghostDelay = 0, interference = 0, hum = 0;
	float verticalHold = 0, jitter = 0, tracking = 0, headSwitch = 0;
	bool interlace = false;

	float time       = 0.0f;
	float frameIndex = 0.0f;
};

/// Stage 2: encode, damage, decode. One line's worth of machinery per pixel,
/// mirroring kSignalFragment exactly.
class SignalStage
{
public:
	SignalStage( const Plane4& src, const SignalSettings& s ) :
		source( src ),
		st( s )
	{
	}

	void run( Plane4& out ) const
	{
		out.resize( st.signalW, st.signalH );

		parallelRows( st.signalH, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				float* row    = out.row( y );
				const float v = ( y + 0.5f ) / st.signalH;

				for( int x = 0; x < st.signalW; ++x )
					pixel( ( x + 0.5f ) / st.signalW, v, &row[ size_t( x ) * 4 ] );
			}
		} );
	}

private:
	const Plane4& source;
	const SignalSettings& st;

	float encode( const float rgb[ 4 ], float phase, float vSign ) const
	{
		const Vec3 yuv = rgbToYuv( rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] );
		return yuv.x + yuv.y * std::sin( phase ) + yuv.z * vSign * std::cos( phase );
	}

	float impairments( float s, float lineIdx, float noiseGain ) const
	{
		float n = ( rnd( s, lineIdx, st.frameIndex ) - 0.5f ) * noiseGain;

		n += st.interference * 0.09f
			 * std::sin( ( s * 0.2405f + lineIdx * 0.91f ) * TAU + st.time * 8.0f );
		n += st.hum * 0.075f
			 * std::sin( ( lineIdx / st.signalH * 1.3f + st.time * 0.21f ) * TAU );

		if( st.dropouts > 0.0f )
		{
			const float hit    = rnd( lineIdx, st.frameIndex, 17.0f ) >= 1.0f - st.dropouts * 0.09f ? 1.0f : 0.0f;
			const float start  = rnd( lineIdx, st.frameIndex, 29.0f ) * st.signalW;
			const float run    = ( 0.01f + 0.09f * rnd( lineIdx, st.frameIndex, 31.0f ) ) * st.signalW;
			const float inside = hit * ( s >= start ? 1.0f : 0.0f ) * ( s <= start + run ? 1.0f : 0.0f );
			n += inside * ( 0.7f + 0.6f * ( rnd( s, lineIdx, st.frameIndex + 3.0f ) - 0.5f ) );
		}

		return n;
	}

	void decodeLine( float lineIdx, float yCoord, float xCoord, float noiseGain,
					 Vec3& yuv, float& alpha ) const
	{
		const float vSign =
			( st.pal && std::fmod( lineIdx, 2.0f ) >= 1.0f ) ? -1.0f : 1.0f;

		const float basePhase = st.linePhaseStep * lineIdx + st.framePhaseStep * st.frameIndex;
		const float texelX    = 1.0f / st.signalW;
		const float sampleIdx = xCoord * st.signalW;

		float ySum = 0, uSum = 0, vSum = 0, aSum = 0;
		float yW = 0, cW = 0;

		for( int i = -TAPS; i <= TAPS; ++i )
		{
			const float fi    = float( i );
			const float spx   = xCoord + fi * texelX;
			const float phase = basePhase + ( sampleIdx + fi ) * st.samplePhaseStep;

			float src[ 4 ];
			bilinear4( source, spx, yCoord, src );
			float comp = encode( src, phase, vSign );

			if( st.ghostAmount > 0.0f )
			{
				float ghost[ 4 ];
				bilinear4( source, spx - st.ghostDelay * texelX, yCoord, ghost );
				const float gPhase = phase - st.ghostDelay * st.samplePhaseStep;
				comp += st.ghostAmount * 0.6f * encode( ghost, gPhase, vSign );
			}

			comp += impairments( sampleIdx + fi, lineIdx, noiseGain );

			const float plain = windowedSinc( fi, st.lumaCutoff );
			const float notch = 0.25f * windowedSinc( fi - 2.0f, st.lumaCutoff )
								+ 0.50f * plain
								+ 0.25f * windowedSinc( fi + 2.0f, st.lumaCutoff );
			const float wy = lerpf( plain, notch, st.notchAmount );

			const float wc = windowedSinc( fi - st.chromaDelay, st.chromaCutoff );

			ySum += comp * wy;
			aSum += src[ 3 ] * wy;
			yW += wy;

			uSum += comp * std::sin( phase + st.phaseError ) * wc;
			vSum += comp * std::cos( phase + st.phaseError ) * wc;
			cW += wc;
		}

		const float yNorm = std::max( yW, 1e-5f );
		const float cNorm = std::max( cW, 1e-5f );

		yuv   = { ySum / yNorm, 2.0f * uSum / cNorm, 2.0f * vSum / cNorm * vSign };
		alpha = aSum / yNorm;
	}

	void pixel( float u, float v, float out[ 4 ] ) const
	{
		const float texelY = 1.0f / st.signalH;

		const float srcY    = fractf( v + st.verticalHold * st.time * 0.65f );
		const float lineIdx = std::floor( srcY * st.signalH );
		const float lineRnd = rnd( lineIdx, st.frameIndex, 5.0f ) - 0.5f;

		const float bandCentre = fractf( st.time * 0.13f + 0.62f );
		const float bandWidth  = 0.015f + st.tracking * 0.10f;
		float bandDist         = std::abs( srcY - bandCentre );
		bandDist               = std::min( bandDist, 1.0f - bandDist );
		const float band       = st.tracking * ( 1.0f - smoothstepf( 0.0f, bandWidth, bandDist ) );

		const float switchAt   = 1.0f - st.headSwitch * 0.07f;
		const float headSwitch = st.headSwitch * smoothstepf( switchAt, switchAt + 0.004f, srcY );

		const float xShift = st.jitter * lineRnd * 0.035f
							 + band * lineRnd * 0.16f
							 + headSwitch * ( 0.06f + lineRnd * 0.05f )
							 + st.hum * 0.004f
								   * std::sin( ( srcY * 1.3f + st.time * 0.21f ) * TAU + 1.2f );

		const float srcX = u + xShift;

		const float noiseGain  = st.noise * 0.20f + band * 0.9f + headSwitch * 0.7f;
		const float chromaKill = std::clamp( band * 1.6f + headSwitch * 1.4f, 0.0f, 1.0f );

		Vec3 yuv;
		float alpha;
		decodeLine( lineIdx, srcY, srcX, noiseGain, yuv, alpha );

		if( st.pal )
		{
			Vec3 yuvAbove;
			float alphaAbove;
			decodeLine( lineIdx - 1.0f, srcY - texelY, srcX, noiseGain, yuvAbove, alphaAbove );
			yuv.y = 0.5f * ( yuv.y + yuvAbove.y );
			yuv.z = 0.5f * ( yuv.z + yuvAbove.z );
		}

		yuv.y *= st.saturation * ( 1.0f - chromaKill );
		yuv.z *= st.saturation * ( 1.0f - chromaKill );

		float rgb[ 3 ];
		yuvToRgb( yuv, rgb );

		if( st.verticalHold > 0.0f )
		{
			const float bar = 1.0f - smoothstepf( 0.0f, st.verticalHold * 0.045f, srcY );
			const float snow =
				0.04f + 0.12f * rnd( u * st.signalW, lineIdx, st.frameIndex + 9.0f );
			for( float* c : { &rgb[ 0 ], &rgb[ 1 ], &rgb[ 2 ] } )
				*c = lerpf( *c, snow, bar );
		}

		if( st.interlace )
		{
			const float fieldWeight =
				std::abs( std::fmod( lineIdx, 2.0f ) - std::fmod( st.frameIndex, 2.0f ) ) < 0.5f
					? 1.0f
					: 0.55f;
			rgb[ 0 ] *= fieldWeight;
			rgb[ 1 ] *= fieldWeight;
			rgb[ 2 ] *= fieldWeight;
		}

		out[ 0 ] = rgb[ 0 ];
		out[ 1 ] = rgb[ 1 ];
		out[ 2 ] = rgb[ 2 ];
		out[ 3 ] = std::clamp( alpha, 0.0f, 1.0f );
	}
};

//---------------------------------------------------------------------------
// Everything the tube pass needs.
//---------------------------------------------------------------------------
struct TubeSettings
{
	int signalW = 754, signalH = 486;
	int outW = 1920, outH = 1080;

	int maskPattern    = 1;
	float maskPitch    = 6.0f;
	float maskStrength = 0.6f;
	float maskSpill    = 0.25f;
	float maskGain     = 1.2f;

	float scanlines  = 0.5f;
	float beamBloom  = 0.5f;
	float halation   = 0.2f;
	float brightness = 1.0f;
	float contrast   = 1.0f;

	float curvature    = 0.15f;
	float cornerRadius = 0.05f;
	float perspectiveX = 0.0f;
	float perspectiveY = 0.0f;
	float zoom         = 1.0f;
	float vignette     = 0.35f;
};

struct FrameSetup
{
	Plane4 signal; //!< after the phosphor
	Plane4 bloom;  //!< quarter size, blurred bright pass
	TubeSettings tube;
};

/// Mirrors kTubeFragment's dotMask.
inline void dotMask( float mcx, float mcy, int pattern, float out[ 3 ] )
{
	out[ 0 ] = out[ 1 ] = out[ 2 ] = 1.0f;

	auto phosphor = []( float idx, float spot, float out[ 3 ] ) {
		out[ 0 ] = idx < 0.5f ? spot : 0.0f;
		out[ 1 ] = idx >= 0.5f && idx < 1.5f ? spot : 0.0f;
		out[ 2 ] = idx >= 1.5f ? spot : 0.0f;
	};

	if( pattern == 1 )
	{
		const float rowHeight = 0.866f;
		const float row       = std::floor( mcy / rowHeight );
		const float x         = mcx + std::fmod( std::abs( row ), 2.0f ) * 0.5f;
		const float idx       = std::fmod( std::abs( std::floor( x ) ), 3.0f );
		const float cx        = fractf( x ) - 0.5f;
		const float cy        = ( fractf( mcy / rowHeight ) - 0.5f ) * rowHeight;
		const float spot      = 1.0f - smoothstepf( 0.24f, 0.46f, std::sqrt( cx * cx + cy * cy ) );
		phosphor( idx, spot, out );
	}
	else if( pattern == 2 )
	{
		const float idx    = std::fmod( std::abs( std::floor( mcx ) ), 3.0f );
		const float stripe = 1.0f - smoothstepf( 0.26f, 0.50f, std::abs( fractf( mcx ) - 0.5f ) );
		phosphor( idx, stripe, out );
	}
	else if( pattern == 3 )
	{
		const float slotHeight = 2.0f;
		const float idx        = std::fmod( std::abs( std::floor( mcx ) ), 3.0f );
		const float stagger    = std::fmod( std::abs( std::floor( mcx / 3.0f ) ), 2.0f ) * 0.5f;
		const float sy         = fractf( mcy / slotHeight + stagger );
		const float stripe     = 1.0f - smoothstepf( 0.28f, 0.50f, std::abs( fractf( mcx ) - 0.5f ) );
		const float slot       = 1.0f - smoothstepf( 0.38f, 0.50f, std::abs( sy - 0.5f ) );
		phosphor( idx, stripe * slot, out );
	}
	else if( pattern == 4 )
	{
		const float idx = std::fmod( std::abs( std::floor( mcx ) ), 3.0f );
		phosphor( idx, 1.0f, out );
	}
}

/// The view/curvature geometry of the tube pass, separated so derivatives can
/// be taken by finite difference — the GPU had fwidth for that.
struct TubeGeometry
{
	float tubeX = 0, tubeY = 0;      //!< point on the face, -1..1 per axis
	float signalU = 0, signalV = 0;  //!< where in the raster it samples
	float lineF = 0;                 //!< scan line coordinate
	float maskX = 0, maskY = 0;      //!< phosphor lattice coordinate
	float sd = 0;                    //!< rounded-rectangle SDF of the face
	float infront = 1;
};

TubeGeometry tubeGeometry( float u, float v, const TubeSettings& t,
						   float cosRX, float sinRX, float cosRY, float sinRY )
{
	constexpr float FOCAL = 2.4f;

	const float aspect = float( t.outW ) / std::max( 1, t.outH );

	TubeGeometry g;

	float px = ( u * 2.0f - 1.0f ) * aspect;
	float py = v * 2.0f - 1.0f;

	//dir = (p / zoom, FOCAL); orientation = rotY(PerspectiveX) * rotX(PerspectiveY).
	const float dx = px / std::max( t.zoom, 0.05f );
	const float dy = py / std::max( t.zoom, 0.05f );
	const float dz = FOCAL;

	//normal = orientation * (0,0,1). With GLSL column-major construction:
	//rotX(a)*(0,0,1) = (0, sin a, cos a); rotY(b)*that = (-sin b * cos a, sin a, cos b * cos a).
	const float nx = -sinRY * cosRX;
	const float ny = sinRX;
	const float nz = cosRY * cosRX;

	float denom = nx * dx + ny * dy + nz * dz;
	denom       = denom >= 0.0f ? std::max( denom, 1e-4f ) : std::min( denom, -1e-4f );
	const float tHit = ( nz * FOCAL ) / denom;

	//local = transpose(orientation) * (t*dir - centre)
	const float wx = tHit * dx;
	const float wy = tHit * dy;
	const float wz = tHit * dz - FOCAL;

	//transpose(rotY(b)*rotX(a)) = transpose(rotX(a)) * transpose(rotY(b)).
	//First undo the Y rotation, then the X rotation.
	const float ux = cosRY * wx - sinRY * wz;
	const float uz1 = sinRY * wx + cosRY * wz;
	const float uy = cosRX * wy - sinRX * uz1;

	g.tubeX   = ux / aspect;
	g.tubeY   = uy;
	g.infront = tHit > 1e-4f ? 1.0f : 0.0f;

	const float r2              = g.tubeX * g.tubeX + g.tubeY * g.tubeY;
	const float cornerExpansion = 1.0f + t.curvature;
	const float curveScale      = ( 1.0f + t.curvature * 0.5f * r2 ) / cornerExpansion;
	const float curvedX         = g.tubeX * curveScale;
	const float curvedY         = g.tubeY * curveScale;

	g.signalU = curvedX * 0.5f + 0.5f;
	g.signalV = curvedY * 0.5f + 0.5f;
	g.lineF   = g.signalV * t.signalH - 0.5f;

	g.maskX = ( g.tubeX * 0.5f + 0.5f ) * t.outW / std::max( t.maskPitch, 1.0f );
	g.maskY = ( g.tubeY * 0.5f + 0.5f ) * t.outH / std::max( t.maskPitch, 1.0f );

	const float radius = std::max( t.cornerRadius, 0.001f );
	const float qx     = std::abs( g.tubeX ) - ( 1.0f - radius );
	const float qy     = std::abs( g.tubeY ) - ( 1.0f - radius );
	const float mx     = std::max( qx, 0.0f );
	const float my     = std::max( qy, 0.0f );
	g.sd = std::sqrt( mx * mx + my * my ) + std::min( std::max( qx, qy ), 0.0f ) - radius;

	return g;
}

class TubeProcessorBase : public OFX::ImageProcessor
{
public:
	explicit TubeProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( const FrameSetup* v, bool premultipliedValue )
	{
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	const FrameSetup* setup = nullptr;
	bool premultiplied      = false;
};

template<class PIX, int nComponents, int maxValue>
class TubeProcessor : public TubeProcessorBase
{
public:
	explicit TubeProcessor( OFX::ImageEffect& effect ) :
		TubeProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		const TubeSettings& t = setup->tube;

		const float cosRX = std::cos( t.perspectiveY ), sinRX = std::sin( t.perspectiveY );
		const float cosRY = std::cos( t.perspectiveX ), sinRY = std::sin( t.perspectiveX );

		const float du = 1.0f / t.outW;
		const float dv = 1.0f / t.outH;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix   = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );
			const float v = ( y - bounds.y1 + 0.5f ) / t.outH;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( x - bounds.x1 + 0.5f ) / t.outW;

				const TubeGeometry g  = tubeGeometry( u, v, t, cosRX, sinRX, cosRY, sinRY );
				const TubeGeometry gx = tubeGeometry( u + du, v, t, cosRX, sinRX, cosRY, sinRY );
				const TubeGeometry gy = tubeGeometry( u, v + dv, t, cosRX, sinRX, cosRY, sinRY );

				//fwidth(): |ddx| + |ddy|, exactly as GLSL defines it.
				const float fwLineF = std::abs( gx.lineF - g.lineF ) + std::abs( gy.lineF - g.lineF );
				const float fwMaskX = std::abs( gx.maskX - g.maskX ) + std::abs( gy.maskX - g.maskX );
				const float fwMaskY = std::abs( gx.maskY - g.maskY ) + std::abs( gy.maskY - g.maskY );
				const float fwSd    = std::abs( gx.sd - g.sd ) + std::abs( gy.sd - g.sd );

				//--- 3. Scan -------------------------------------------------
				const float base = std::floor( g.lineF );

				const float pixelsPerLine = 1.0f / std::max( fwLineF, 1e-5f );
				const float scanAA        = smoothstepf( 1.2f, 2.0f, pixelsPerLine );

				float beamSum[ 3 ] = { 0, 0, 0 };
				float alphaSum = 0, weightSum = 0, weightFlat = 0;

				for( int i = -1; i <= 1; ++i )
				{
					const float li = base + float( i );
					float c[ 4 ];
					bilinear4( setup->signal, g.signalU, ( li + 0.5f ) / t.signalH, c );

					const float luma = std::clamp(
						0.299f * std::abs( c[ 0 ] ) + 0.587f * std::abs( c[ 1 ] ) + 0.114f * std::abs( c[ 2 ] ),
						0.0f, 1.0f );
					const float sigma = lerpf( 0.26f, lerpf( 0.34f, 0.95f, t.beamBloom ), luma );

					const float d     = g.lineF - li;
					const float w     = std::exp( -0.5f * d * d / ( sigma * sigma ) );
					const float wFlat = std::exp( -0.5f * float( i ) * float( i ) / ( sigma * sigma ) );

					beamSum[ 0 ] += c[ 0 ] * w;
					beamSum[ 1 ] += c[ 1 ] * w;
					beamSum[ 2 ] += c[ 2 ] * w;
					alphaSum += c[ 3 ] * w;
					weightSum += w;
					weightFlat += wFlat;
				}

				float colour[ 3 ];
				for( int k = 0; k < 3; ++k )
					colour[ k ] = beamSum[ k ] / std::max( weightSum, 1e-4f );
				float alpha         = alphaSum / std::max( weightSum, 1e-4f );
				const float scanMod = weightSum / std::max( weightFlat, 1e-4f );
				const float scanMul = lerpf( 1.0f, std::min( scanMod, 1.0f ), t.scanlines * scanAA );
				for( float& c : colour )
					c *= scanMul;

				//--- halation, then the front panel --------------------------
				float halo[ 4 ];
				bilinear4( setup->bloom, g.signalU, g.signalV, halo );
				for( int k = 0; k < 3; ++k )
				{
					colour[ k ] += halo[ k ] * t.halation;
					colour[ k ] = ( colour[ k ] - 0.5f ) * t.contrast + 0.5f;
					colour[ k ] *= t.brightness;
				}

				//--- 4. Mask --------------------------------------------------
				const float dotsPerPixel = std::max( fwMaskX, fwMaskY );
				const float maskAA       = 1.0f - smoothstepf( 0.4f, 0.8f, dotsPerPixel );

				float shape[ 3 ];
				dotMask( g.maskX, g.maskY, t.maskPattern, shape );
				const float strength = t.maskStrength * maskAA;
				for( int k = 0; k < 3; ++k )
				{
					const float shaped = lerpf( t.maskSpill, 1.0f, shape[ k ] );
					colour[ k ] *= lerpf( 1.0f, shaped * t.maskGain, strength );
				}

				if( t.maskPattern == 2 )
				{
					float wire = 0.0f;
					const float ww = 1.6f / t.outH * 2.0f;
					wire += 1.0f - smoothstepf( 0.0f, ww, std::abs( g.tubeY - 0.36f ) );
					wire += 1.0f - smoothstepf( 0.0f, ww, std::abs( g.tubeY + 0.36f ) );
					const float cut = 1.0f - std::clamp( wire, 0.0f, 1.0f ) * 0.28f * strength;
					for( float& c : colour )
						c *= cut;
				}

				//--- the edge of the glass -------------------------------------
				const float rim = std::sqrt( g.tubeX * 0.92f * g.tubeX * 0.92f + g.tubeY * g.tubeY );
				const float vignette = 1.0f - t.vignette * smoothstepf( 0.25f, 1.5f, rim );
				for( float& c : colour )
					c *= vignette;

				const float aa   = std::max( fwSd, 1e-4f );
				float face       = 1.0f - smoothstepf( -aa, aa, g.sd );
				const bool outside = g.signalU < 0.0f || g.signalV < 0.0f || g.signalU > 1.0f || g.signalV > 1.0f;
				face *= ( outside ? 0.0f : 1.0f ) * g.infront;

				alpha = std::clamp( alpha, 0.0f, 1.0f ) * face;

				double r = std::clamp( colour[ 0 ], 0.0f, 1.0f ) * alpha;
				double gOut = std::clamp( colour[ 1 ], 0.0f, 1.0f ) * alpha;
				double b = std::clamp( colour[ 2 ], 0.0f, 1.0f ) * alpha;
				double a = alpha;

				if( !premultiplied && nComponents == 4 && a > 0.0 )
				{
					r /= a;
					gOut /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( gOut );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

constexpr const char* kParamSystem       = "system";
constexpr const char* kParamSource       = "signalSource";
constexpr const char* kParamLumaBw       = "lumaBandwidth";
constexpr const char* kParamChromaBw     = "chromaBandwidth";
constexpr const char* kParamSaturation   = "saturation";
constexpr const char* kParamTint         = "tint";
constexpr const char* kParamDotCrawl     = "dotCrawl";
constexpr const char* kParamGhosting     = "ghosting";
constexpr const char* kParamGhostDelay   = "ghostDelay";
constexpr const char* kParamNoise        = "noise";
constexpr const char* kParamDropouts     = "dropouts";
constexpr const char* kParamInterference = "interference";
constexpr const char* kParamVerticalHold = "verticalHold";
constexpr const char* kParamJitter       = "jitter";
constexpr const char* kParamTracking     = "tracking";
constexpr const char* kParamHeadSwitch   = "headSwitch";
constexpr const char* kParamHum          = "hum";
constexpr const char* kParamInterlace    = "interlace";
constexpr const char* kParamMaskPattern  = "maskPattern";
constexpr const char* kParamMaskPitch    = "maskPitch";
constexpr const char* kParamMaskStrength = "maskStrength";
constexpr const char* kParamScanlines    = "scanlines";
constexpr const char* kParamBeamBloom    = "beamBloom";
constexpr const char* kParamPersistence  = "persistence";
constexpr const char* kParamHalation     = "halation";
constexpr const char* kParamBrightness   = "brightness";
constexpr const char* kParamContrast     = "contrast";
constexpr const char* kParamCurvature    = "curvature";
constexpr const char* kParamCornerRadius = "cornerRadius";
constexpr const char* kParamPerspectiveX = "perspectiveX";
constexpr const char* kParamPerspectiveY = "perspectiveY";
constexpr const char* kParamZoom         = "zoom";
constexpr const char* kParamVignette     = "vignette";

class OldCathodePlugin : public OFX::ImageEffect
{
public:
	explicit OldCathodePlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		systemP      = fetchChoiceParam( kParamSystem );
		sourceP      = fetchChoiceParam( kParamSource );
		lumaBw       = fetchDoubleParam( kParamLumaBw );
		chromaBw     = fetchDoubleParam( kParamChromaBw );
		saturation   = fetchDoubleParam( kParamSaturation );
		tint         = fetchDoubleParam( kParamTint );
		dotCrawl     = fetchDoubleParam( kParamDotCrawl );
		ghosting     = fetchDoubleParam( kParamGhosting );
		ghostDelay   = fetchDoubleParam( kParamGhostDelay );
		noise        = fetchDoubleParam( kParamNoise );
		dropouts     = fetchDoubleParam( kParamDropouts );
		interference = fetchDoubleParam( kParamInterference );
		verticalHold = fetchDoubleParam( kParamVerticalHold );
		jitter       = fetchDoubleParam( kParamJitter );
		tracking     = fetchDoubleParam( kParamTracking );
		headSwitch   = fetchDoubleParam( kParamHeadSwitch );
		hum          = fetchDoubleParam( kParamHum );
		interlace    = fetchBooleanParam( kParamInterlace );
		maskPattern  = fetchChoiceParam( kParamMaskPattern );
		maskPitch    = fetchDoubleParam( kParamMaskPitch );
		maskStrength = fetchDoubleParam( kParamMaskStrength );
		scanlines    = fetchDoubleParam( kParamScanlines );
		beamBloom    = fetchDoubleParam( kParamBeamBloom );
		persistence  = fetchDoubleParam( kParamPersistence );
		halation     = fetchDoubleParam( kParamHalation );
		brightness   = fetchDoubleParam( kParamBrightness );
		contrast     = fetchDoubleParam( kParamContrast );
		curvature    = fetchDoubleParam( kParamCurvature );
		cornerRadius = fetchDoubleParam( kParamCornerRadius );
		perspectiveX = fetchDoubleParam( kParamPerspectiveX );
		perspectiveY = fetchDoubleParam( kParamPerspectiveY );
		zoom         = fetchDoubleParam( kParamZoom );
		vignette     = fetchDoubleParam( kParamVignette );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src( srcClip->fetchImage( args.time ) );

		const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		FrameSetup setup;
		buildSetup( args, *dst, *src, premultiplied, setup );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? runTube<TubeProcessor<unsigned char, 4, 255>>( args, dst.get(), setup, premultiplied )
				: runTube<TubeProcessor<unsigned char, 3, 255>>( args, dst.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? runTube<TubeProcessor<unsigned short, 4, 65535>>( args, dst.get(), setup, premultiplied )
				: runTube<TubeProcessor<unsigned short, 3, 65535>>( args, dst.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? runTube<TubeProcessor<float, 4, 1>>( args, dst.get(), setup, premultiplied )
				: runTube<TubeProcessor<float, 3, 1>>( args, dst.get(), setup, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void getFramesNeeded( const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames ) override
	{
		OfxRangeD range;
		range.min = args.time - historyWindow( args.time );
		range.max = args.time;
		frames.setFramesNeeded( *srcClip, range );
	}

private:
	int historyWindow( double t ) const
	{
		const float decay = float( persistence->getValueAtTime( t ) ) * 0.93f;
		if( decay <= 0.001f )
			return 0;
		//decay^k < 1/255 -> k, capped.
		const int k = int( std::ceil( std::log( 1.0 / 255.0 ) / std::log( double( decay ) ) ) );
		return std::clamp( k, 0, kMaxHistory );
	}

	/// Stage 1: onto the standard's raster, band-limited, straight colour.
	static void resample( OFX::Image& img, bool premultiplied, int signalW, int signalH, Plane4& out )
	{
		const OfxRectI b = img.getBounds();
		const int srcW   = b.x2 - b.x1;
		const int srcH   = b.y2 - b.y1;
		out.resize( signalW, signalH );

		const OFX::BitDepthEnum depth       = img.getPixelDepth();
		const OFX::PixelComponentEnum comps = img.getPixelComponents();
		const int n                         = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		parallelRows( signalH, [ & ]( int y0, int y1 ) {
			for( int y = y0; y < y1; ++y )
			{
				float* row = out.row( y );
				const int py0 = int( double( y ) * srcH / signalH );
				const int py1 = std::max( py0 + 1, int( double( y + 1 ) * srcH / signalH ) );

				for( int x = 0; x < signalW; ++x )
				{
					const int px0 = int( double( x ) * srcW / signalW );
					const int px1 = std::max( px0 + 1, int( double( x + 1 ) * srcW / signalW ) );

					double sum[ 4 ] = { 0, 0, 0, 0 };
					int count       = 0;
					for( int sy = py0; sy < py1; ++sy )
					{
						for( int sx = px0; sx < px1; ++sx )
						{
							const void* pix =
								img.getPixelAddress( b.x1 + std::min( sx, srcW - 1 ),
													 b.y1 + std::min( sy, srcH - 1 ) );
							if( !pix )
								continue;
							double r, g, bl, a;
							switch( depth )
							{
							case OFX::eBitDepthUByte:
							{
								const unsigned char* p = static_cast<const unsigned char*>( pix );
								r  = p[ 0 ] / 255.0;
								g  = p[ 1 ] / 255.0;
								bl = p[ 2 ] / 255.0;
								a  = n == 4 ? p[ 3 ] / 255.0 : 1.0;
								break;
							}
							case OFX::eBitDepthUShort:
							{
								const unsigned short* p = static_cast<const unsigned short*>( pix );
								r  = p[ 0 ] / 65535.0;
								g  = p[ 1 ] / 65535.0;
								bl = p[ 2 ] / 65535.0;
								a  = n == 4 ? p[ 3 ] / 65535.0 : 1.0;
								break;
							}
							default:
							{
								const float* p = static_cast<const float*>( pix );
								r  = p[ 0 ];
								g  = p[ 1 ];
								bl = p[ 2 ];
								a  = n == 4 ? p[ 3 ] : 1.0;
								break;
							}
							}
							if( !premultiplied && n == 4 )
							{
								r *= a;
								g *= a;
								bl *= a;
							}
							sum[ 0 ] += r;
							sum[ 1 ] += g;
							sum[ 2 ] += bl;
							sum[ 3 ] += a;
							++count;
						}
					}
					if( count > 0 )
						for( double& s : sum )
							s /= count;

					//Everything downstream works in straight colour: the
					//encoder measures luminance, and a premultiplied pixel
					//that is dark only because it is transparent would be
					//encoded as a legitimately dark picture.
					if( sum[ 3 ] > 0.0 )
					{
						sum[ 0 ] /= sum[ 3 ];
						sum[ 1 ] /= sum[ 3 ];
						sum[ 2 ] /= sum[ 3 ];
					}

					row[ x * 4 + 0 ] = float( sum[ 0 ] );
					row[ x * 4 + 1 ] = float( sum[ 1 ] );
					row[ x * 4 + 2 ] = float( sum[ 2 ] );
					row[ x * 4 + 3 ] = float( sum[ 3 ] );
				}
			}
		} );
	}

	void buildSetup( const OFX::RenderArguments& args, OFX::Image& dst, OFX::Image& src,
					 bool premultiplied, FrameSetup& setup )
	{
		using namespace oldcathode;

		const double t = args.time;

		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;

		int systemIdx = 0, sourceIdx = 0;
		systemP->getValueAtTime( t, systemIdx );
		sourceP->getValueAtTime( t, sourceIdx );
		const SystemSpec& sys = system( systemIdx );
		const SourceSpec& srcSpec = source( sourceIdx );

		//--- the signal settings, derived exactly as the FFGL build derives them
		SignalSettings sig;
		sig.signalW         = sys.signalWidth();
		sig.signalH         = sys.signalHeight();
		sig.samplePhaseStep = sys.samplePhaseStep();
		sig.linePhaseStep   = sys.linePhaseStep();
		sig.framePhaseStep  = sys.framePhaseStep();
		sig.pal             = sys.pal;

		const float lumaMHz = ( srcSpec.lumaMHz > 0.0f ? srcSpec.lumaMHz : sys.nominalLumaMHz )
							  * lerpf( 0.25f, 1.25f, float( lumaBw->getValueAtTime( t ) ) );
		const float chromaMHz = ( srcSpec.chromaMHz > 0.0f ? srcSpec.chromaMHz : sys.nominalChromaMHz )
								* lerpf( 0.25f, 1.25f, float( chromaBw->getValueAtTime( t ) ) );
		const float toNormalised = sys.activeLineMicroseconds / float( sig.signalW );

		sig.lumaCutoff   = std::clamp( lumaMHz * toNormalised, 0.01f, 0.49f );
		sig.chromaCutoff = std::clamp( chromaMHz * toNormalised, 0.004f, 0.49f );
		sig.chromaDelay  = srcSpec.chromaDelaySamples;
		sig.notchAmount  = 1.0f - float( dotCrawl->getValueAtTime( t ) );
		sig.saturation   = float( saturation->getValueAtTime( t ) ) * 2.0f;
		sig.phaseError   = ( float( tint->getValueAtTime( t ) ) - 0.5f ) * 1.4f;

		sig.noise        = float( noise->getValueAtTime( t ) ) * srcSpec.noiseScale;
		sig.dropouts     = float( dropouts->getValueAtTime( t ) );
		sig.ghostAmount  = float( ghosting->getValueAtTime( t ) );
		sig.ghostDelay   = float( ghostDelay->getValueAtTime( t ) ) * 48.0f;
		sig.interference = float( interference->getValueAtTime( t ) );
		sig.hum          = float( hum->getValueAtTime( t ) );

		sig.verticalHold = float( verticalHold->getValueAtTime( t ) );
		sig.jitter       = float( jitter->getValueAtTime( t ) );
		sig.tracking     = float( tracking->getValueAtTime( t ) );
		sig.headSwitch   = float( headSwitch->getValueAtTime( t ) );
		sig.interlace    = interlace->getValueAtTime( t );

		//--- the signal at this frame ------------------------------------------
		auto signalAt = [ & ]( OFX::Image& frame, double frameTime, Plane4& out ) {
			Plane4 sd;
			resample( frame, premultiplied, sig.signalW, sig.signalH, sd );

			SignalSettings frameSig = sig;
			frameSig.time           = float( frameTime / fps );
			frameSig.frameIndex     = float( std::fmod( frameTime, 100000.0 ) );

			SignalStage stage( sd, frameSig );
			stage.run( out );
		};

		signalAt( src, t, setup.signal );

		//--- phosphor decay, reconstructed from previous frames -----------------
		const float decayBase = float( persistence->getValueAtTime( t ) ) * 0.93f;
		const int window      = historyWindow( t );
		if( window > 0 )
		{
			const float decay[ 3 ] = { decayBase * 0.97f, decayBase, decayBase * 0.90f };
			const float maxDecay   = std::max( decay[ 0 ], std::max( decay[ 1 ], decay[ 2 ] ) );

			for( int k = 1; k <= window; ++k )
			{
				std::unique_ptr<OFX::Image> past( srcClip->fetchImage( t - k ) );
				if( !past )
					break;

				Plane4 pastSignal;
				signalAt( *past, t - k, pastSignal );
				if( pastSignal.w != setup.signal.w || pastSignal.h != setup.signal.h )
					break;

				//max(current, faded history), folded with decay^k.
				float fade[ 3 ];
				for( int c = 0; c < 3; ++c )
					fade[ c ] = std::pow( decay[ c ], float( k ) );
				const float fadeA = std::pow( maxDecay, float( k ) );

				float* cur        = setup.signal.v.data();
				const float* past0 = pastSignal.v.data();
				const size_t count = setup.signal.v.size() / 4;
				for( size_t i = 0; i < count; ++i, cur += 4, past0 += 4 )
				{
					cur[ 0 ] = std::max( cur[ 0 ], past0[ 0 ] * fade[ 0 ] );
					cur[ 1 ] = std::max( cur[ 1 ], past0[ 1 ] * fade[ 1 ] );
					cur[ 2 ] = std::max( cur[ 2 ], past0[ 2 ] * fade[ 2 ] );
					cur[ 3 ] = std::max( cur[ 3 ], past0[ 3 ] * fadeA );
				}
			}
		}

		//--- halation: bright pass + separable blur at quarter size -------------
		const float halationV = float( halation->getValueAtTime( t ) );
		const int bloomW      = std::max( 1, sig.signalW / 4 );
		const int bloomH      = std::max( 1, sig.signalH / 4 );
		setup.bloom.resize( bloomW, bloomH );

		if( halationV > 0.001f )
		{
			Plane4 bright;
			bright.resize( bloomW, bloomH );
			const float texelX = 1.0f / sig.signalW;
			const float texelY = 1.0f / sig.signalH;

			parallelRows( bloomH, [ & ]( int y0, int y1 ) {
				for( int y = y0; y < y1; ++y )
				{
					float* row    = bright.row( y );
					const float v = ( y + 0.5f ) / bloomH;
					for( int x = 0; x < bloomW; ++x )
					{
						const float u = ( x + 0.5f ) / bloomW;
						float sum[ 3 ] = { 0, 0, 0 };
						for( int cy = -1; cy <= 1; cy += 2 )
							for( int cx = -1; cx <= 1; cx += 2 )
							{
								float c[ 4 ];
								bilinear4( setup.signal, u + cx * texelX, v + cy * texelY, c );
								sum[ 0 ] += c[ 0 ];
								sum[ 1 ] += c[ 1 ];
								sum[ 2 ] += c[ 2 ];
							}
						const float luma = 0.299f * sum[ 0 ] * 0.25f + 0.587f * sum[ 1 ] * 0.25f
										   + 0.114f * sum[ 2 ] * 0.25f;
						const float gate = smoothstepf( 0.5f, 0.85f, luma );
						float* o         = &row[ size_t( x ) * 4 ];
						o[ 0 ] = std::max( sum[ 0 ] * 0.25f * gate, 0.0f );
						o[ 1 ] = std::max( sum[ 1 ] * 0.25f * gate, 0.0f );
						o[ 2 ] = std::max( sum[ 2 ] * 0.25f * gate, 0.0f );
						o[ 3 ] = 1.0f;
					}
				}
			} );

			const float offsets[ 3 ] = { 0.0f, 1.3846153846f, 3.2307692308f };
			const float weights[ 3 ] = { 0.2270270270f, 0.3162162162f, 0.0702702703f };

			auto blur = [ & ]( const Plane4& from, Plane4& to, float dirX, float dirY ) {
				parallelRows( bloomH, [ & ]( int y0, int y1 ) {
					for( int y = y0; y < y1; ++y )
					{
						float* row    = to.row( y );
						const float v = ( y + 0.5f ) / bloomH;
						for( int x = 0; x < bloomW; ++x )
						{
							const float u = ( x + 0.5f ) / bloomW;
							float sum[ 4 ];
							bilinear4( from, u, v, sum );
							for( int c = 0; c < 4; ++c )
								sum[ c ] *= weights[ 0 ];
							for( int i = 1; i < 3; ++i )
							{
								float c1[ 4 ], c2[ 4 ];
								bilinear4( from, u + dirX * offsets[ i ], v + dirY * offsets[ i ], c1 );
								bilinear4( from, u - dirX * offsets[ i ], v - dirY * offsets[ i ], c2 );
								for( int c = 0; c < 4; ++c )
									sum[ c ] += ( c1[ c ] + c2[ c ] ) * weights[ i ];
							}
							std::memcpy( &row[ size_t( x ) * 4 ], sum, sizeof( sum ) );
						}
					}
				} );
			};

			Plane4 mid;
			mid.resize( bloomW, bloomH );
			blur( bright, mid, 1.0f / bloomW, 0.0f );
			blur( mid, setup.bloom, 0.0f, 1.0f / bloomH );
		}

		//--- the tube ------------------------------------------------------------
		const OfxRectI db = dst.getBounds();
		TubeSettings& tube = setup.tube;
		tube.signalW = sig.signalW;
		tube.signalH = sig.signalH;
		tube.outW    = db.x2 - db.x1;
		tube.outH    = db.y2 - db.y1;

		int maskIdx = 1;
		maskPattern->getValueAtTime( t, maskIdx );
		const MaskSpec& maskSpec = mask( maskIdx );
		tube.maskPattern  = maskIdx;
		tube.maskPitch    = lerpf( 2.0f, 14.0f, float( maskPitch->getValueAtTime( t ) ) );
		tube.maskStrength = float( maskStrength->getValueAtTime( t ) );
		tube.maskSpill    = maskSpec.spill;
		tube.maskGain     = maskSpec.gain;

		tube.scanlines  = float( scanlines->getValueAtTime( t ) );
		tube.beamBloom  = float( beamBloom->getValueAtTime( t ) );
		tube.halation   = halationV > 0.001f ? halationV * 0.8f : 0.0f;
		tube.brightness = float( brightness->getValueAtTime( t ) ) * 2.0f;
		tube.contrast   = float( contrast->getValueAtTime( t ) ) * 2.0f;

		tube.curvature    = float( curvature->getValueAtTime( t ) ) * 0.6f;
		tube.cornerRadius = float( cornerRadius->getValueAtTime( t ) ) * 0.35f;
		tube.perspectiveX = ( float( perspectiveX->getValueAtTime( t ) ) - 0.5f ) * 1.8f;
		tube.perspectiveY = ( float( perspectiveY->getValueAtTime( t ) ) - 0.5f ) * 1.8f;
		tube.zoom         = lerpf( 0.5f, 1.5f, float( zoom->getValueAtTime( t ) ) );
		tube.vignette     = float( vignette->getValueAtTime( t ) );
	}

	template<class Processor>
	void runTube( const OFX::RenderArguments& args, OFX::Image* dst, const FrameSetup& setup,
				  bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( &setup, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* systemP       = nullptr;
	OFX::ChoiceParam* sourceP       = nullptr;
	OFX::DoubleParam* lumaBw        = nullptr;
	OFX::DoubleParam* chromaBw      = nullptr;
	OFX::DoubleParam* saturation    = nullptr;
	OFX::DoubleParam* tint          = nullptr;
	OFX::DoubleParam* dotCrawl      = nullptr;
	OFX::DoubleParam* ghosting      = nullptr;
	OFX::DoubleParam* ghostDelay    = nullptr;
	OFX::DoubleParam* noise         = nullptr;
	OFX::DoubleParam* dropouts      = nullptr;
	OFX::DoubleParam* interference  = nullptr;
	OFX::DoubleParam* verticalHold  = nullptr;
	OFX::DoubleParam* jitter        = nullptr;
	OFX::DoubleParam* tracking      = nullptr;
	OFX::DoubleParam* headSwitch    = nullptr;
	OFX::DoubleParam* hum           = nullptr;
	OFX::BooleanParam* interlace    = nullptr;
	OFX::ChoiceParam* maskPattern   = nullptr;
	OFX::DoubleParam* maskPitch     = nullptr;
	OFX::DoubleParam* maskStrength  = nullptr;
	OFX::DoubleParam* scanlines     = nullptr;
	OFX::DoubleParam* beamBloom     = nullptr;
	OFX::DoubleParam* persistence   = nullptr;
	OFX::DoubleParam* halation      = nullptr;
	OFX::DoubleParam* brightness    = nullptr;
	OFX::DoubleParam* contrast      = nullptr;
	OFX::DoubleParam* curvature     = nullptr;
	OFX::DoubleParam* cornerRadius  = nullptr;
	OFX::DoubleParam* perspectiveX  = nullptr;
	OFX::DoubleParam* perspectiveY  = nullptr;
	OFX::DoubleParam* zoom          = nullptr;
	OFX::DoubleParam* vignette      = nullptr;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( OldCathodePluginFactory, {}, {} );

void OldCathodePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The signal stages run at the SD raster whatever the frame size, so a
	// tile cannot render alone; the phosphor reads previous source frames, so
	// temporal access is declared. Frames still render in any order — the
	// subcarrier phase comes from the frame number, not from history.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( true );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void OldCathodePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	using namespace oldcathode;

	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );
	srcClip->setTemporalClipAccess( true );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	OFX::GroupParamDescriptor* signalGroup = desc.defineGroupParam( "Signal" );
	signalGroup->setLabels( "Signal", "Signal", "Signal" );

	OFX::ChoiceParamDescriptor* systemParam = desc.defineChoiceParam( kParamSystem );
	systemParam->setLabels( "System", "System", "System" );
	systemParam->setHint( "The broadcast standard. The artefacts are consequences of its numbers." );
	for( int i = 0; i < systemCount(); ++i )
		systemParam->appendOption( system( i ).name );
	systemParam->setDefault( 0 );
	systemParam->setParent( *signalGroup );
	page->addChild( *systemParam );

	OFX::ChoiceParamDescriptor* sourceParam = desc.defineChoiceParam( kParamSource );
	sourceParam->setLabels( "Source", "Source", "Source" );
	sourceParam->setHint( "What the picture was played back from. Tape records colour on its own "
						  "low carrier, which is why its colour is soft and slightly late." );
	for( int i = 0; i < sourceCount(); ++i )
		sourceParam->appendOption( source( i ).name );
	sourceParam->setDefault( 0 );
	sourceParam->setParent( *signalGroup );
	page->addChild( *sourceParam );

	defineSlider( desc, page, kParamLumaBw, "Luma Bandwidth", "0.75 is exactly what the standard specifies.", 0.75 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamChromaBw, "Chroma Bandwidth", "", 0.75 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamSaturation, "Saturation", "0.5 is unity.", 0.5 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamTint, "Tint", "Reference phase error; 0.5 is none. PAL trades it for saturation.", 0.5 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamDotCrawl, "Dot Crawl", "How much of the subcarrier the set leaves in the luminance.", 0.6 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamGhosting, "Ghosting", "Multipath: the same transmission off a building, later and weaker.", 0.0 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamGhostDelay, "Ghost Delay", "", 0.3 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamNoise, "Noise", "Added to the wire, so it decodes as grey grain and coloured speckle.", 0.12 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamDropouts, "Dropouts", "A worn tape momentarily losing head contact.", 0.0 )->setParent( *signalGroup );
	defineSlider( desc, page, kParamInterference, "Interference", "A beat from an adjacent channel.", 0.0 )->setParent( *signalGroup );

	OFX::GroupParamDescriptor* syncGroup = desc.defineGroupParam( "Sync" );
	syncGroup->setLabels( "Sync", "Sync", "Sync" );

	defineSlider( desc, page, kParamVerticalHold, "Vertical Hold", "The raster walks and takes the blanking with it.", 0.0 )->setParent( *syncGroup );
	defineSlider( desc, page, kParamJitter, "Jitter", "Line-to-line timebase error.", 0.08 )->setParent( *syncGroup );
	defineSlider( desc, page, kParamTracking, "Tracking", "The band of a mistracking tape, walking vertically.", 0.0 )->setParent( *syncGroup );
	defineSlider( desc, page, kParamHeadSwitch, "Head Switch", "The tear just below the bottom of the picture.", 0.0 )->setParent( *syncGroup );
	defineSlider( desc, page, kParamHum, "Hum", "Mains ripple in the black level and the deflection.", 0.0 )->setParent( *syncGroup );

	OFX::BooleanParamDescriptor* interlaceParam = desc.defineBooleanParam( kParamInterlace );
	interlaceParam->setLabels( "Interlace", "Interlace", "Interlace" );
	interlaceParam->setHint( "Only half the lines are refreshed each frame." );
	interlaceParam->setDefault( false );
	interlaceParam->setParent( *syncGroup );
	page->addChild( *interlaceParam );

	OFX::GroupParamDescriptor* tubeGroup = desc.defineGroupParam( "Tube" );
	tubeGroup->setLabels( "Tube", "Tube", "Tube" );

	OFX::ChoiceParamDescriptor* maskParam = desc.defineChoiceParam( kParamMaskPattern );
	maskParam->setLabels( "Mask Pattern", "Mask Pattern", "Mask Pattern" );
	for( int i = 0; i < maskCount(); ++i )
		maskParam->appendOption( mask( i ).name );
	maskParam->setDefault( 1 );
	maskParam->setParent( *tubeGroup );
	page->addChild( *maskParam );

	defineSlider( desc, page, kParamMaskPitch, "Mask Pitch", "Output pixels per phosphor dot.", 0.35 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamMaskStrength, "Mask Strength", "", 0.6 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamScanlines, "Scanlines", "", 0.5 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamBeamBloom, "Beam Bloom", "A brighter line is a fatter line.", 0.5 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamPersistence, "Persistence",
				  "Emissive decay: blue goes first, green hangs on longest. Rebuilt from "
				  "previous frames here, so high values cost render time.",
				  0.15 )
		->setParent( *tubeGroup );
	defineSlider( desc, page, kParamHalation, "Halation", "Light scattering inside the faceplate.", 0.25 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamBrightness, "Brightness", "0.5 is unity.", 0.5 )->setParent( *tubeGroup );
	defineSlider( desc, page, kParamContrast, "Contrast", "0.5 is unity.", 0.5 )->setParent( *tubeGroup );

	OFX::GroupParamDescriptor* geometryGroup = desc.defineGroupParam( "Geometry" );
	geometryGroup->setLabels( "Geometry", "Geometry", "Geometry" );

	defineSlider( desc, page, kParamCurvature, "Curvature", "", 0.25 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamCornerRadius, "Corner Radius", "", 0.15 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamPerspectiveX, "Perspective X", "0.5 is straight on.", 0.5 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamPerspectiveY, "Perspective Y", "0.5 is straight on.", 0.5 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamZoom, "Zoom", "0.5 is 1:1.", 0.5 )->setParent( *geometryGroup );
	defineSlider( desc, page, kParamVignette, "Vignette", "", 0.35 )->setParent( *geometryGroup );
}

OFX::ImageEffect* OldCathodePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new OldCathodePlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static OldCathodePluginFactory* factory =
		new OldCathodePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
