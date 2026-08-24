#include "../Shaders.h"

namespace oldcathode::shaders
{
/// Encode to composite, damage the composite, decode it back.
///
/// This is the stage that earns the plugin its name, and it is worth being
/// precise about why it is built this way rather than as a pile of overlays.
///
/// A colour television signal is one wire. Luminance occupies it from DC to the
/// channel limit; the two colour-difference signals are put in quadrature on a
/// subcarrier sitting *inside* that same band. Everything that happens to the
/// picture on its way to the set -- noise, echoes, interference, a worn tape --
/// happens to that single combined waveform, and is then un-mixed by a decoder
/// that cannot tell which part of what it received was luminance and which was
/// chrominance.
///
/// That confusion is the entire visual signature of analogue television:
///
///   Dot crawl      the subcarrier is inside the luma band, so unless the set
///                  notches it out it is still there when the picture is drawn,
///                  as a fine chequer that creeps because its phase advances
///                  every line and every frame.
///   Cross-colour   fine luminance detail near the subcarrier frequency is
///                  demodulated as if it were colour, so a herringbone jacket
///                  turns into rainbows.
///   Chroma smear   colour is carried in a fraction of the bandwidth luminance
///                  gets, so it bleeds sideways across edges.
///   Coloured snow  noise added to the composite is un-mixed along with
///                  everything else, so it arrives as grey grain *and* as
///                  coloured speckle, in the right proportion, for free.
///
/// None of those are drawn here. They are consequences. Add the noise before
/// the decoder instead of after it and they appear on their own, which is the
/// difference between modelling the path and painting the symptoms.
const char* const kSignalFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 SignalSize;

//Carrier geometry, all derived on the CPU from the chosen system.
uniform float SamplePhaseStep;//radians of subcarrier per sample, ~PI/2 at 4fsc
uniform float LinePhaseStep;  //radians the carrier advances between lines
uniform float FramePhaseStep; //...and between frames, giving the 4/8-field cycle
uniform float PalMode;        //1.0 = alternate V and run the delay line

//Decoder
uniform float LumaCutoff;  //normalised, cycles per sample
uniform float ChromaCutoff;
uniform float ChromaDelay; //samples the colour lags the luminance by
uniform float NotchAmount; //1.0 removes the subcarrier from luma, killing dot crawl
uniform float Saturation;
uniform float PhaseError;  //radians of error in the local reference

//Impairments
uniform float Noise;
uniform float Dropouts;
uniform float GhostAmount;
uniform float GhostDelay;//samples
uniform float Interference;
uniform float Hum;

//Timebase
uniform float VerticalHold;

//How far the raster has walked, in fractions of a field. NOT `VerticalHold *
//Time`: that is an absolute product, so moving the control an hour into a
//composition jumps the picture by hundreds of fields at once. The host side
//anchors it -- see OldCathode.h -- and hands over the position it has reached.
uniform float VerticalRoll;
uniform float Jitter;
uniform float Tracking;
uniform float HeadSwitch;
uniform float Interlace;

uniform float Time;
uniform float FrameIndex;

in vec2 uv;

out vec4 fragColor;

const float PI  = 3.14159265359;
const float TAU = 6.28318530718;
const int TAPS  = 8;//-> 17-tap filters

//--------------------------------------------------------------------------
// BT.601. This is standard-definition video, so it is 601 and not 709 -- the
// weights are the ones the encoder at the far end would actually have used.
//--------------------------------------------------------------------------
vec3 rgbToYuv( vec3 c )
{
	return vec3(
		dot( c, vec3( 0.299, 0.587, 0.114 ) ),
		dot( c, vec3( -0.14713, -0.28886, 0.436 ) ),
		dot( c, vec3( 0.615, -0.51499, -0.10001 ) ) );
}

vec3 yuvToRgb( vec3 c )
{
	return vec3(
		c.x + 1.13983 * c.z,
		c.x - 0.39465 * c.y - 0.58060 * c.z,
		c.x + 2.03211 * c.y );
}

//--------------------------------------------------------------------------
// A well-mixed integer hash. Standing in for the thermal noise of the tuner
// front end, which is white and uncorrelated sample to sample. Integer rather
// than the usual fract(sin(...)) because that loses precision at large
// arguments on some drivers and starts producing visible structure -- and
// structured noise reads as a bug, not as snow.
//--------------------------------------------------------------------------
uint hashU( uvec3 v )
{
	uint h = v.x * 374761393u + v.y * 668265263u + v.z * 2246822519u;
	h = ( h ^ ( h >> 13u ) ) * 1274126177u;
	return h ^ ( h >> 16u );
}

float rnd( float a, float b, float c )
{
	uvec3 k = uvec3( ivec3( floor( vec3( a, b, c ) ) ) );
	return float( hashU( k ) & 0x00FFFFFFu ) / 16777216.0;
}

//--------------------------------------------------------------------------
// Windowed sinc. Deliberately short: a real set's chroma take-off filter is a
// two or three pole affair with a very gentle skirt, so a 17-tap approximation
// is closer to the hardware than a long brick-wall one would be. n may be
// fractional, which is how the chroma path gets its sub-sample delay.
//--------------------------------------------------------------------------
float windowedSinc( float n, float cutoff )
{
	float x = 2.0 * cutoff * n;
	float s = abs( x ) < 1e-6 ? 1.0 : sin( PI * x ) / ( PI * x );
	float w = max( 0.5 + 0.5 * cos( PI * n / float( TAPS + 1 ) ), 0.0 );
	return 2.0 * cutoff * s * w;
}

float encode( vec3 rgb, float phase, float vSign )
{
	vec3 yuv = rgbToYuv( rgb );
	return yuv.x + yuv.y * sin( phase ) + yuv.z * vSign * cos( phase );
}

//--------------------------------------------------------------------------
// Everything that is added to the wire rather than to the picture. Called once
// per filter tap so that all of it goes through the decoder, which is what
// turns flat white noise into the grey-plus-colour speckle a weak signal
// actually produces.
//--------------------------------------------------------------------------
float impairments( float s, float lineIdx, float noiseGain )
{
	float n = ( rnd( s, lineIdx, FrameIndex ) - 0.5 ) * noiseGain;

	//A beat from an adjacent channel or a nearby oscillator. Placed just off the
	//subcarrier so it walks slowly rather than standing still.
	n += Interference * 0.09 * sin( ( s * 0.2405 + lineIdx * 0.91 ) * TAU + Time * 8.0 );

	//Mains ripple reaching the video amplifier: a slow standing wave in the
	//black level. The field rate and the mains are close but not locked, so the
	//bar drifts instead of sitting still.
	n += Hum * 0.075 * sin( ( lineIdx / SignalSize.y * 1.3 + Time * 0.21 ) * TAU );

	//A worn tape momentarily loses head contact. The RF vanishes for a few
	//microseconds and there is nothing left to demodulate.
	if( Dropouts > 0.0 )
	{
		float hit    = step( 1.0 - Dropouts * 0.09, rnd( lineIdx, FrameIndex, 17.0 ) );
		float start  = rnd( lineIdx, FrameIndex, 29.0 ) * SignalSize.x;
		float run = ( 0.01 + 0.09 * rnd( lineIdx, FrameIndex, 31.0 ) ) * SignalSize.x;
		float inside = hit * step( start, s ) * step( s, start + run );
		n += inside * ( 0.7 + 0.6 * ( rnd( s, lineIdx, FrameIndex + 3.0 ) - 0.5 ) );
	}

	return n;
}

//--------------------------------------------------------------------------
// One line, all the way there and back: encode every sample in the filter's
// support to composite, wreck it, and run a synchronous demodulator over the
// result. The luma and chroma paths differ only in their filter weights, which
// is exactly how it works in the set.
//--------------------------------------------------------------------------
void decodeLine( float lineIdx, float yCoord, float xCoord, float noiseGain,
                 out vec3 yuv, out float alpha )
{
	//PAL inverts V on alternate lines. NTSC does not, which is why NTSC has a
	//hue control on the front panel and PAL does not.
	float vSign = ( PalMode > 0.5 && mod( lineIdx, 2.0 ) >= 1.0 ) ? -1.0 : 1.0;

	float basePhase = LinePhaseStep * lineIdx + FramePhaseStep * FrameIndex;
	float texelX    = 1.0 / SignalSize.x;
	float sampleIdx = xCoord * SignalSize.x;

	float ySum = 0.0, uSum = 0.0, vSum = 0.0, aSum = 0.0;
	float yW = 0.0, cW = 0.0;

	for( int i = -TAPS; i <= TAPS; ++i )
	{
		float fi    = float( i );
		vec2 sp     = vec2( xCoord + fi * texelX, yCoord );
		float phase = basePhase + ( sampleIdx + fi ) * SamplePhaseStep;

		vec4 src   = texture( SourceTexture, sp );
		float comp = encode( src.rgb, phase, vSign );

		//Multipath: the same transmission arriving a second time off a building,
		//later and weaker. It is added on the wire, before the decoder, so the
		//echo carries its own displaced colour too.
		if( GhostAmount > 0.0 )
		{
			vec3 ghost   = texture( SourceTexture, vec2( sp.x - GhostDelay * texelX, sp.y ) ).rgb;
			float gPhase = phase - GhostDelay * SamplePhaseStep;
			comp += GhostAmount * 0.6 * encode( ghost, gPhase, vSign );
		}

		comp += impairments( sampleIdx + fi, lineIdx, noiseGain );

		//Luma path. A plain low-pass leaves the subcarrier sitting in the
		//luminance, which is dot crawl; sets that wanted rid of it notched it
		//out and lost the detail above 3MHz in exchange. At 4x subcarrier
		//sampling, averaging samples two apart is exactly that notch -- they sit
		//180 degrees apart, so the carrier cancels and nothing else does.
		float plain = windowedSinc( fi, LumaCutoff );
		float notch = 0.25 * windowedSinc( fi - 2.0, LumaCutoff )
		            + 0.50 * plain
		            + 0.25 * windowedSinc( fi + 2.0, LumaCutoff );
		float wy = mix( plain, notch, NotchAmount );

		//Chroma path. The offset centre is the colour-under delay of a tape
		//machine: chrominance is recorded on its own low carrier and comes back
		//a little later than the luminance it belongs to.
		float wc = windowedSinc( fi - ChromaDelay, ChromaCutoff );

		ySum += comp * wy;
		aSum += src.a * wy;
		yW += wy;

		//Synchronous demodulation against the receiver's own reference. Any
		//error in that reference rotates every hue on the line.
		uSum += comp * sin( phase + PhaseError ) * wc;
		vSum += comp * cos( phase + PhaseError ) * wc;
		cW += wc;
	}

	float yNorm = max( yW, 1e-5 );
	float cNorm = max( cW, 1e-5 );

	//The factor of two undoes the average of sin^2 over the carrier.
	yuv   = vec3( ySum / yNorm, 2.0 * uSum / cNorm, 2.0 * vSum / cNorm * vSign );
	alpha = aSum / yNorm;
}

void main()
{
	float texelY = 1.0 / SignalSize.y;

	//----------------------------------------------------------------------
	// Where the beam actually is, as opposed to where it should be.
	//----------------------------------------------------------------------

	//Vertical hold: the field no longer starts where the flyback expects it to,
	//so the whole raster walks and takes the blanking interval with it.
	float srcY = fract( uv.y + VerticalRoll );

	float lineIdx = floor( srcY * SignalSize.y );
	float lineRnd = rnd( lineIdx, FrameIndex, 5.0 ) - 0.5;

	//Tracking: the head is no longer centred on the recorded track. RF level
	//collapses in a band that walks vertically as the error beats against the
	//drum, and either side of it the servo's correction shows as a skew.
	float bandCentre = fract( Time * 0.13 + 0.62 );
	float bandWidth  = 0.015 + Tracking * 0.10;
	float bandDist   = abs( srcY - bandCentre );
	bandDist         = min( bandDist, 1.0 - bandDist );
	float band       = Tracking * ( 1.0 - smoothstep( 0.0, bandWidth, bandDist ) );

	//Head switch: the point in the field where playback changes heads. It sits
	//just below the bottom of a correctly set-up picture, which is why it is
	//normally invisible and why it tears when it is not.
	float switchAt   = 1.0 - HeadSwitch * 0.07;
	float headSwitch = HeadSwitch * smoothstep( switchAt, switchAt + 0.004, srcY );

	//Line-to-line timebase error: each line starts a little early or late.
	float xShift = Jitter * lineRnd * 0.035
	             + band * lineRnd * 0.16
	             + headSwitch * ( 0.06 + lineRnd * 0.05 )
	             //The same mains ripple that bends the black level also reaches
	             //the horizontal deflection, so the left edge bows with it.
	             + Hum * 0.004 * sin( ( srcY * 1.3 + Time * 0.21 ) * TAU + 1.2 );

	float srcX = uv.x + xShift;

	//Losing RF costs the colour before it costs the picture: chrominance sits
	//at the top of the band and is the first thing under the noise floor.
	float noiseGain = Noise * 0.20 + band * 0.9 + headSwitch * 0.7;
	float chromaKill = clamp( band * 1.6 + headSwitch * 1.4, 0.0, 1.0 );

	//----------------------------------------------------------------------
	// There and back.
	//----------------------------------------------------------------------
	vec3 yuv;
	float alpha;
	decodeLine( lineIdx, srcY, srcX, noiseGain, yuv, alpha );

	if( PalMode > 0.5 )
	{
		//The delay line. A reference error rotates this line's colour one way
		//and the line above it the other, because V was transmitted inverted, so
		//averaging the pair cancels the rotation and costs a little saturation
		//instead. Trading hue error for saturation error is the whole reason PAL
		//exists, and it is why the same Tint control does something quite
		//different here than it does in NTSC.
		vec3 yuvAbove;
		float alphaAbove;
		decodeLine( lineIdx - 1.0, srcY - texelY, srcX, noiseGain, yuvAbove, alphaAbove );
		yuv.yz = 0.5 * ( yuv.yz + yuvAbove.yz );
	}

	yuv.yz *= Saturation * ( 1.0 - chromaKill );

	vec3 rgb = yuvToRgb( yuv );

	//----------------------------------------------------------------------
	// Blanking and interlace.
	//----------------------------------------------------------------------

	//When the picture rolls, the join has to show something, and what it shows
	//is the interval between fields: no picture, just whatever the noise floor
	//is doing.
	if( VerticalHold > 0.0 )
	{
		float bar = 1.0 - smoothstep( 0.0, VerticalHold * 0.045, srcY );
		rgb = mix( rgb, vec3( 0.04 + 0.12 * rnd( uv.x * SignalSize.x, lineIdx, FrameIndex + 9.0 ) ), bar );
	}

	//Interlace: only half the lines are refreshed this frame. The other field is
	//a frame stale and has had time to fade, which is what makes fine horizontal
	//detail twitter at half the frame rate on a real set.
	if( Interlace > 0.5 )
	{
		float fieldWeight = abs( mod( lineIdx, 2.0 ) - mod( FrameIndex, 2.0 ) ) < 0.5 ? 1.0 : 0.55;
		rgb *= fieldWeight;
	}

	fragColor = vec4( rgb, clamp( alpha, 0.0, 1.0 ) );
}
)";
} // namespace oldcathode::shaders
