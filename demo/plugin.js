/**
 * Old Cathode — browser demo.
 *
 * All six shaders below are copied unedited from `source/shaders/` — Vertex,
 * Resample, Signal, Phosphor, Bloom, Blur and Tube. The pass structure, the
 * buffer formats and every uniform value are ports of `ProcessOpenGL` in
 * `source/OldCathode.cpp`, and the system/source/mask tables are ports of
 * `source/Standards.cpp`.
 *
 * The thing worth understanding before reading any of it: this is an analogue
 * television **signal path**, not a CRT look. It encodes the picture onto a
 * colour subcarrier, damages the composite as one signal, and decodes it with a
 * synchronous demodulator. Dot crawl, cross-colour, chroma smear and coloured
 * snow are consequences of that chain, never drawn. Turn Dot Crawl down and you
 * are notching the subcarrier out of the luminance and losing the fine detail
 * along with it, because that is the trade the real sets made.
 *
 * The signal stages run at the standard's own raster — 754 x 486 for NTSC,
 * 921 x 576 for PAL — whatever the composition size is. 754 is 4x subcarrier,
 * which is what makes the decoder's chroma notch a plain three-tap average.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//===========================================================================
// Ports of source/Standards.cpp
//===========================================================================

const TAU = 6.28318530718;

// name, subcarrierMHz, subcarrierCyclesPerLine, linesPerFrame, activeLines,
// activeLineMicroseconds, nominalLumaMHz, nominalChromaMHz, pal
const SYSTEMS = [
  {
    name: 'NTSC',
    subcarrierMHz: 3.579545, subcarrierCyclesPerLine: 227.5, linesPerFrame: 525,
    activeLines: 486, activeLineMicroseconds: 52.6557,
    nominalLumaMHz: 4.2, nominalChromaMHz: 1.3, pal: false,
  },
  {
    // PAL-I, the 5.5MHz variant used in the UK and Ireland.
    name: 'PAL',
    subcarrierMHz: 4.43361875, subcarrierCyclesPerLine: 283.7516, linesPerFrame: 625,
    activeLines: 576, activeLineMicroseconds: 51.9479,
    nominalLumaMHz: 5.5, nominalChromaMHz: 1.3, pal: true,
  },
];

const SOURCES_SPEC = [
  { name: 'Broadcast', lumaMHz: 0.0, chromaMHz: 0.0, chromaDelaySamples: 0.0, noiseScale: 1.0 },
  { name: 'VHS SP', lumaMHz: 3.0, chromaMHz: 0.4, chromaDelaySamples: 1.6, noiseScale: 1.8 },
  { name: 'VHS LP', lumaMHz: 2.4, chromaMHz: 0.4, chromaDelaySamples: 2.2, noiseScale: 2.6 },
  { name: 'VHS EP', lumaMHz: 2.0, chromaMHz: 0.35, chromaDelaySamples: 2.8, noiseScale: 3.4 },
];

// The gains are measured, not calculated. Do not tidy them into round numbers.
const MASKS = [
  { name: 'None', spill: 0.0, gain: 1.0 },
  { name: 'Shadow Mask', spill: 0.35, gain: 2.256 },
  { name: 'Aperture Grille', spill: 0.28, gain: 2.148 },
  { name: 'Slot Mask', spill: 0.32, gain: 2.079 },
  { name: 'RGB Stripe', spill: 0.15, gain: 2.308 },
];

// How far the raster has walked, carried forward across a Vertical Hold change.
//
// The roll used to be VerticalHold * Time in the shader. That moves it by
// Time * delta the instant the control changes, and here Time is how long the
// page has been open -- and because fract wraps the result, the picture does not
// speed up, it lands somewhere arbitrary. Dragging the slider read as the raster
// teleporting rather than the hold slipping. Mirrors OldCathode.h.
let rollAnchor = 0;
let rollAnchorTime = 0;
let rollAnchorHold = -1;

function verticalRoll(hold, seconds) {
  if (rollAnchorHold < 0) {
    // First frame: anchor stays at zero, so this is exactly the old product
    // until the control is touched.
    rollAnchorHold = hold;
  } else if (hold !== rollAnchorHold) {
    // Once per change, not once per frame.
    rollAnchor += (seconds - rollAnchorTime) * rollAnchorHold * 0.65;
    rollAnchorTime = seconds;
    rollAnchorHold = hold;
  }

  return rollAnchor + (seconds - rollAnchorTime) * hold * 0.65;
}

const signalWidth = (sys) => Math.round(4 * sys.subcarrierMHz * sys.activeLineMicroseconds);
const signalHeight = (sys) => sys.activeLines;

const samplePhaseStep = (sys) =>
  (TAU * (sys.subcarrierMHz * sys.activeLineMicroseconds)) / signalWidth(sys);

/// Only the fractional part matters: a whole cycle between lines is no shift.
const linePhaseStep = (sys) => TAU * (sys.subcarrierCyclesPerLine % 1);

/// NTSC lands on half a cycle per frame (the four-field sequence); PAL on three
/// quarters (eight). Both fall out of the arithmetic rather than being cased.
const framePhaseStep = (sys) =>
  TAU * ((sys.subcarrierCyclesPerLine * sys.linesPerFrame) % 1);

const lerp = (a, b, t) => a + (b - a) * t;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

//===========================================================================
// Shaders — verbatim from source/shaders/
//===========================================================================

const VERTEX = `#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
`;

const RESAMPLE = `#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	//One tap per source texel covered, capped so a 4K or 8K composition costs a
	//bounded amount. The cap only bites past an 8:1 reduction, and this pass
	//runs at SD, so even the worst case is a few million fetches.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 6.0 ) ) );
	vec2 texel = MaxUV / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			//Spread the taps evenly across this destination sample's footprint.
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, uv + f * ratio * texel );
		}
	}

	vec4 color = sum / float( taps.x * taps.y );

	//Everything downstream works in straight colour. The encoder measures
	//luminance, and a premultiplied pixel that is dark only because it is
	//transparent would otherwise be encoded as a legitimately dark picture.
	if( color.a > 0.0 )
		color.rgb /= color.a;

	fragColor = color;
}
`;

const SIGNAL = `#version 410 core
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

//How far the raster has walked, in fractions of a field. NOT VerticalHold * Time
//-- an absolute product jumps the picture to an unrelated offset the instant the
//control moves, because the roll wraps. The page side anchors it and hands over
//the position reached. Mirrors OldCathode.h. (No backticks in here: this shader
//lives in a JS template literal, and one would end it.)
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
`;

const PHOSPHOR = `#version 410 core
uniform sampler2D CurrentTexture;
uniform sampler2D HistoryTexture;
uniform vec3 Decay;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec4 current = texture( CurrentTexture, uv );
	vec4 history = texture( HistoryTexture, uv );

	vec3 faded = history.rgb * Decay;
	float maxDecay = max( Decay.r, max( Decay.g, Decay.b ) );

	fragColor = vec4( max( current.rgb, faded ), max( current.a, history.a * maxDecay ) );
}
`;

const BLOOM = `#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 SourceSize;
uniform float Threshold;

in vec2 uv;

out vec4 fragColor;

void main()
{
	//Four bilinear taps at the corners of the destination footprint: a box
	//downsample that does not leave stair-stepping in the halo.
	vec2 texel = 1.0 / max( SourceSize, vec2( 1.0 ) );
	vec3 sum = texture( SourceTexture, uv + vec2( -1.0, -1.0 ) * texel ).rgb
	         + texture( SourceTexture, uv + vec2( 1.0, -1.0 ) * texel ).rgb
	         + texture( SourceTexture, uv + vec2( -1.0, 1.0 ) * texel ).rgb
	         + texture( SourceTexture, uv + vec2( 1.0, 1.0 ) * texel ).rgb;
	vec3 color = sum * 0.25;

	float luma = dot( color, vec3( 0.299, 0.587, 0.114 ) );
	color *= smoothstep( Threshold, Threshold + 0.35, luma );

	fragColor = vec4( max( color, vec3( 0.0 ) ), 1.0 );
}
`;

const BLUR = `#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 Direction;//one texel along the axis being blurred

in vec2 uv;

out vec4 fragColor;

void main()
{
	const float offsets[ 3 ] = float[]( 0.0, 1.3846153846, 3.2307692308 );
	const float weights[ 3 ] = float[]( 0.2270270270, 0.3162162162, 0.0702702703 );

	vec3 sum = texture( SourceTexture, uv ).rgb * weights[ 0 ];
	for( int i = 1; i < 3; ++i )
	{
		sum += texture( SourceTexture, uv + Direction * offsets[ i ] ).rgb * weights[ i ];
		sum += texture( SourceTexture, uv - Direction * offsets[ i ] ).rgb * weights[ i ];
	}

	fragColor = vec4( sum, 1.0 );
}
`;

const TUBE = `#version 410 core
uniform sampler2D SignalTexture;
uniform sampler2D BloomTexture;
uniform vec2 SignalSize;
uniform vec2 OutputSize;

uniform float MaskPattern;
uniform float MaskPitch;//output pixels per phosphor dot
uniform float MaskStrength;
uniform float MaskSpill;
uniform float MaskGain;

uniform float Scanlines;
uniform float BeamBloom;
uniform float Halation;
uniform float Brightness;
uniform float Contrast;

uniform float Curvature;
uniform float CornerRadius;
uniform float PerspectiveX;
uniform float PerspectiveY;
uniform float Zoom;
uniform float Vignette;

in vec2 uv;

out vec4 fragColor;

const float FOCAL = 2.4;

mat3 rotationX( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( 1.0, 0.0, 0.0,
	             0.0, c, s,
	             0.0, -s, c );
}

mat3 rotationY( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( c, 0.0, -s,
	             0.0, 1.0, 0.0,
	             s, 0.0, c );
}

//--------------------------------------------------------------------------
// The phosphor layout, as the transmission of each of the three phosphors at
// this point. Coordinates are in dot pitches, so one unit is one phosphor in
// both axes whatever the pitch is set to.
//--------------------------------------------------------------------------
vec3 dotMask( vec2 mc, int pattern )
{
	if( pattern == 1 )
	{
		//Delta shadow mask: round phosphor dots on a hexagonal lattice, every
		//other row offset by half a triad. The consumer television mask.
		const float rowHeight = 0.866;//sqrt(3)/2 -- what makes the lattice regular
		float row  = floor( mc.y / rowHeight );
		float x    = mc.x + mod( row, 2.0 ) * 0.5;
		float idx  = mod( floor( x ), 3.0 );
		vec2 cell  = vec2( fract( x ) - 0.5, ( fract( mc.y / rowHeight ) - 0.5 ) * rowHeight );
		float spot = 1.0 - smoothstep( 0.24, 0.46, length( cell ) );
		vec3 phos  = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos * spot;
	}

	if( pattern == 2 )
	{
		//Aperture grille: continuous vertical phosphor stripes held apart by
		//tensioned wires rather than a perforated sheet. Brighter than a shadow
		//mask because far less of the beam is intercepted -- hence the smaller
		//gain -- and with no vertical structure at all.
		float idx    = mod( floor( mc.x ), 3.0 );
		float stripe = 1.0 - smoothstep( 0.26, 0.50, abs( fract( mc.x ) - 0.5 ) );
		vec3 phos    = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos * stripe;
	}

	if( pattern == 3 )
	{
		//Slot mask: the compromise. Stripes broken into slots so the sheet keeps
		//its rigidity, with alternate triads staggered vertically. What most
		//later televisions and cheap monitors actually used.
		const float slotHeight = 2.0;
		float idx    = mod( floor( mc.x ), 3.0 );
		float stagger = mod( floor( mc.x / 3.0 ), 2.0 ) * 0.5;
		float sy     = fract( mc.y / slotHeight + stagger );
		float stripe = 1.0 - smoothstep( 0.28, 0.50, abs( fract( mc.x ) - 0.5 ) );
		float slot   = 1.0 - smoothstep( 0.38, 0.50, abs( sy - 0.5 ) );
		vec3 phos    = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos * stripe * slot;
	}

	if( pattern == 4 )
	{
		//Hard RGB stripe: no gap, no vertical structure. Not a mask any tube
		//ever had, but it is what a coarse subpixel grid looks like and it stays
		//legible at pitches where the others have turned to mush.
		float idx = mod( floor( mc.x ), 3.0 );
		vec3 phos = idx < 0.5 ? vec3( 1.0, 0.0, 0.0 ) : ( idx < 1.5 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		return phos;
	}

	return vec3( 1.0 );
}

void main()
{
	float aspect = OutputSize.x / max( OutputSize.y, 1.0 );

	//----------------------------------------------------------------------
	// 1. Undo the view.
	//----------------------------------------------------------------------
	vec2 p = uv * 2.0 - 1.0;
	p.x *= aspect;//square units, so a rotation is a rotation

	mat3 orientation = rotationY( PerspectiveX ) * rotationX( PerspectiveY );
	vec3 dir     = vec3( p / max( Zoom, 0.05 ), FOCAL );
	vec3 normal  = orientation * vec3( 0.0, 0.0, 1.0 );
	vec3 centre  = vec3( 0.0, 0.0, FOCAL );

	//Guarded rather than branched: an early return here would leave the
	//derivatives below undefined for the whole quad, and the scanline and mask
	//anti-aliasing both depend on them.
	float denom = dot( normal, dir );
	denom = denom >= 0.0 ? max( denom, 1e-4 ) : min( denom, -1e-4 );
	float t = dot( normal, centre ) / denom;

	vec3 local  = transpose( orientation ) * ( t * dir - centre );
	vec2 tube   = vec2( local.x / aspect, local.y );
	float infront = step( 1e-4, t );//the face is behind the eye at absurd angles

	//----------------------------------------------------------------------
	// 2. Undo the curvature. The face bulges, so the sampling pinches.
	//----------------------------------------------------------------------
	//
	// Divided through by the expansion at the corner, which is overscan: a set
	// deliberately scans a raster larger than its own tube face, so the picture
	// reaches the bezel on all four sides and the blanking edges stay hidden
	// behind it. Without it the distortion pulls the picture's own corners
	// inside the glass and shows black beyond them -- the one thing a correctly
	// set-up television never does.
	float cornerExpansion = 1.0 + Curvature;//0.5 * dot( tube, tube ) at the corner is 1
	vec2 curved = tube * ( 1.0 + Curvature * 0.5 * dot( tube, tube ) ) / cornerExpansion;
	vec2 signalUV = curved * 0.5 + 0.5;

	//----------------------------------------------------------------------
	// 3. Scan.
	//----------------------------------------------------------------------
	float lineF = signalUV.y * SignalSize.y - 0.5;//integer at line centres
	float base  = floor( lineF );

	//How many output pixels one scan line covers. Below roughly one and a bit
	//there is nothing left to draw and the modulation is pure aliasing, so it
	//fades out. 486 lines into a 1080-line composition is 2.2 pixels each,
	//which is the most common case there is, and it has to be on the
	//full-strength side of this.
	float pixelsPerLine = 1.0 / max( fwidth( lineF ), 1e-5 );
	float scanAA = smoothstep( 1.2, 2.0, pixelsPerLine );

	vec3 beamSum = vec3( 0.0 );
	float alphaSum = 0.0;
	float weightSum = 0.0;
	float weightFlat = 0.0;

	for( int i = -1; i <= 1; ++i )
	{
		float li = base + float( i );
		vec4 c   = texture( SignalTexture, vec2( signalUV.x, ( li + 0.5 ) / SignalSize.y ) );

		//A brighter line is a fatter line. More beam current means a bigger spot
		//on the phosphor, which is why highlights on a CRT swell into the gaps
		//between scan lines and shadows do not.
		float luma  = clamp( dot( abs( c.rgb ), vec3( 0.299, 0.587, 0.114 ) ), 0.0, 1.0 );
		float sigma = mix( 0.26, mix( 0.34, 0.95, BeamBloom ), luma );

		float d = lineF - li;
		float w = exp( -0.5 * d * d / ( sigma * sigma ) );
		//The same profile sampled dead-on, which is what the line would be worth
		//if the beam were centred here. Their ratio is the scan modulation and
		//is independent of the picture, so strength 0 returns the picture intact.
		float wFlat = exp( -0.5 * float( i ) * float( i ) / ( sigma * sigma ) );

		beamSum += c.rgb * w;
		alphaSum += c.a * w;
		weightSum += w;
		weightFlat += wFlat;
	}

	vec3 color   = beamSum / max( weightSum, 1e-4 );
	float alpha  = alphaSum / max( weightSum, 1e-4 );
	float scanMod = weightSum / max( weightFlat, 1e-4 );
	color *= mix( 1.0, min( scanMod, 1.0 ), Scanlines * scanAA );

	//----------------------------------------------------------------------
	// Halation, then the picture controls, in that order: the front panel
	// adjusts the beam, and the glass scatters whatever the beam produced.
	//----------------------------------------------------------------------
	color += texture( BloomTexture, signalUV ).rgb * Halation;
	color = ( color - 0.5 ) * Contrast + 0.5;
	color *= Brightness;

	//----------------------------------------------------------------------
	// 4. Mask.
	//----------------------------------------------------------------------
	int pattern = int( MaskPattern + 0.5 );
	vec2 maskCoord = ( tube * 0.5 + 0.5 ) * OutputSize / max( MaskPitch, 1.0 );

	//Once a phosphor is smaller than a pixel the mask is no longer a mask, it is
	//a moire generator. Fade it out on the derivative rather than on a resolution
	//check, so it also does the right thing when perspective shrinks the tube.
	vec2 maskRate = fwidth( maskCoord );
	float dotsPerPixel = max( maskRate.x, maskRate.y );
	float maskAA = 1.0 - smoothstep( 0.4, 0.8, dotsPerPixel );

	//The spot is wider than one phosphor, so its neighbours are always partly
	//lit. That floor is why a real mask reads as a texture over the picture
	//rather than as three separated primaries.
	vec3 shape = mix( vec3( MaskSpill ), vec3( 1.0 ), dotMask( maskCoord, pattern ) );
	float strength = MaskStrength * maskAA;
	color *= mix( vec3( 1.0 ), shape * MaskGain, strength );

	//Damper wires. Two of them, and they are only on a grille -- the shadow
	//masks do not need them because a perforated sheet holds itself up. They are
	//the giveaway that you are looking at a Trinitron.
	if( pattern == 2 )
	{
		float wire = 0.0;
		wire += 1.0 - smoothstep( 0.0, 1.6 / OutputSize.y * 2.0, abs( tube.y - 0.36 ) );
		wire += 1.0 - smoothstep( 0.0, 1.6 / OutputSize.y * 2.0, abs( tube.y + 0.36 ) );
		color *= 1.0 - clamp( wire, 0.0, 1.0 ) * 0.28 * strength;
	}

	//----------------------------------------------------------------------
	// The edge of the glass.
	//----------------------------------------------------------------------
	float vignette = 1.0 - Vignette * smoothstep( 0.25, 1.5, length( tube * vec2( 0.92, 1.0 ) ) );
	color *= vignette;

	//A rounded rectangle in the tube's own coordinates, so the bezel keeps its
	//shape when the set is turned away from you.
	float radius = max( CornerRadius, 0.001 );
	vec2 q  = abs( tube ) - vec2( 1.0 - radius );
	float sd = length( max( q, vec2( 0.0 ) ) ) + min( max( q.x, q.y ), 0.0 ) - radius;
	float aa = max( fwidth( sd ), 1e-4 );
	float face = 1.0 - smoothstep( -aa, aa, sd );

	//Curvature can push the sample outside the raster before the face mask cuts
	//it off; there is no picture out there.
	vec2 outside = step( vec2( 0.0 ), -signalUV ) + step( vec2( 1.0 ), signalUV );
	face *= ( 1.0 - clamp( outside.x + outside.y, 0.0, 1.0 ) ) * infront;

	alpha = clamp( alpha, 0.0, 1.0 ) * face;
	fragColor = vec4( clamp( color, vec3( 0.0 ), vec3( 1.0 ) ) * alpha, alpha );
}
`;

//===========================================================================
// The renderer — a port of ProcessOpenGL in source/OldCathode.cpp
//===========================================================================

function createRenderer(gl, quad) {
  const resampleShader = new Program(gl, VERTEX, RESAMPLE, 'resample');
  const signalShader = new Program(gl, VERTEX, SIGNAL, 'signal');
  const phosphorShader = new Program(gl, VERTEX, PHOSPHOR, 'phosphor');
  const bloomShader = new Program(gl, VERTEX, BLOOM, 'bloom');
  const blurShader = new Program(gl, VERTEX, BLUR, 'blur');
  const tubeShader = new Program(gl, VERTEX, TUBE, 'tube');

  const resampleBuffer = new PassBuffer(gl);
  const signalBuffer = new PassBuffer(gl);
  const phosphorBuffer = [new PassBuffer(gl), new PassBuffer(gl)];
  const bloomBuffer = [new PassBuffer(gl), new PassBuffer(gl), new PassBuffer(gl)];
  let phosphorIndex = 0;

  return {
    render({ input, params, width, height, time, frameIndex }) {
      const RGBA16F = gl.RGBA16F;

      const sys = SYSTEMS[Math.round(params.get('system'))];
      const src = SOURCES_SPEC[Math.round(params.get('source'))];

      const signalW = signalWidth(sys);
      const signalH = signalHeight(sys);
      const bloomW = Math.max(1, Math.floor(signalW / 4));
      const bloomH = Math.max(1, Math.floor(signalH / 4));

      // 16-bit float rather than 8-bit: the composite waveform swings outside
      // 0..1 once chroma is riding on it, and quantising it to 256 levels
      // mid-chain would band the decode.
      resampleBuffer.ensure(signalW, signalH, RGBA16F);
      signalBuffer.ensure(signalW, signalH, RGBA16F);
      phosphorBuffer[0].ensure(signalW, signalH, RGBA16F);
      phosphorBuffer[1].ensure(signalW, signalH, RGBA16F);
      bloomBuffer[0].ensure(bloomW, bloomH, RGBA16F);
      bloomBuffer[1].ensure(bloomW, bloomH, RGBA16F);
      bloomBuffer[2].ensure(bloomW, bloomH, RGBA16F);

      const usePhosphor = params.get('persistence') > 0.001;
      const useHalation = params.get('halation') > 0.001;

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. Onto the standard's raster, band-limited on the way.
      //------------------------------------------------------------------
      resampleBuffer.bind();
      resampleShader.use();
      bindTexture(gl, 0, input.texture);
      resampleShader.setSampler('InputTexture', 0);
      resampleShader.set('MaxUV', 1, 1);
      resampleShader.set('InputSize', input.width, input.height);
      resampleShader.set('TargetSize', signalW, signalH);
      quad.draw();

      //------------------------------------------------------------------
      // 2. Encode, damage, decode.
      //------------------------------------------------------------------
      const lumaMHz =
        (src.lumaMHz > 0 ? src.lumaMHz : sys.nominalLumaMHz) *
        lerp(0.25, 1.25, params.get('lumaBandwidth'));
      const chromaMHz =
        (src.chromaMHz > 0 ? src.chromaMHz : sys.nominalChromaMHz) *
        lerp(0.25, 1.25, params.get('chromaBandwidth'));

      // MHz -> cycles per active line -> cycles per sample, which is what the
      // filter kernels want.
      const toNormalised = sys.activeLineMicroseconds / signalW;

      signalBuffer.bind();
      signalShader.use();
      bindTexture(gl, 0, resampleBuffer.texture);
      signalShader.setSampler('SourceTexture', 0);
      signalShader.set('MaxUV', 1, 1);
      signalShader.set('SignalSize', signalW, signalH);

      signalShader.set('SamplePhaseStep', samplePhaseStep(sys));
      signalShader.set('LinePhaseStep', linePhaseStep(sys));
      signalShader.set('FramePhaseStep', framePhaseStep(sys));
      signalShader.set('PalMode', sys.pal ? 1 : 0);

      signalShader.set('LumaCutoff', clamp(lumaMHz * toNormalised, 0.01, 0.49));
      signalShader.set('ChromaCutoff', clamp(chromaMHz * toNormalised, 0.004, 0.49));
      signalShader.set('ChromaDelay', src.chromaDelaySamples);
      signalShader.set('NotchAmount', 1 - params.get('dotCrawl'));
      signalShader.set('Saturation', params.get('saturation') * 2);
      // Plus or minus forty degrees, about as far as a receiver's hue control
      // would swing before the picture stopped being watchable.
      signalShader.set('PhaseError', (params.get('tint') - 0.5) * 1.4);

      signalShader.set('Noise', params.get('noise') * src.noiseScale);
      signalShader.set('Dropouts', params.get('dropouts'));
      signalShader.set('GhostAmount', params.get('ghosting'));
      signalShader.set('GhostDelay', params.get('ghostDelay') * 48);
      signalShader.set('Interference', params.get('interference'));
      signalShader.set('Hum', params.get('hum'));

      signalShader.set('VerticalHold', params.get('verticalHold'));
      // The anchored walk. The control itself still goes over as well, because
      // the rolling bar's width is an amplitude and wants the raw value.
      signalShader.set('VerticalRoll', verticalRoll(params.get('verticalHold'), time));
      signalShader.set('Jitter', params.get('jitter'));
      signalShader.set('Tracking', params.get('tracking'));
      signalShader.set('HeadSwitch', params.get('headSwitch'));
      signalShader.set('Interlace', params.get('interlace'));

      signalShader.set('Time', time);
      signalShader.set('FrameIndex', frameIndex);
      quad.draw();

      //------------------------------------------------------------------
      // 3. Phosphor decay. Skipped entirely at zero.
      //------------------------------------------------------------------
      let displayTexture = signalBuffer.texture;
      if (usePhosphor) {
        const target = phosphorIndex;
        const history = 1 - phosphorIndex;

        phosphorBuffer[target].bind();
        phosphorShader.use();
        bindTexture(gl, 0, signalBuffer.texture);
        bindTexture(gl, 1, phosphorBuffer[history].texture);
        phosphorShader.setSampler('CurrentTexture', 0);
        phosphorShader.setSampler('HistoryTexture', 1);
        phosphorShader.set('MaxUV', 1, 1);

        const decay = params.get('persistence') * 0.93;
        // Blue goes first, green hangs on longest — a white object dragged
        // across the screen leaves a faintly green wake, which is the detail
        // that makes the trail read as a tube rather than a feedback buffer.
        phosphorShader.set('Decay', decay * 0.97, decay, decay * 0.9);
        quad.draw();

        displayTexture = phosphorBuffer[target].texture;
        phosphorIndex = history;
      }

      //------------------------------------------------------------------
      // 4. Halation: bright pass, then a separable blur, all at quarter size.
      //------------------------------------------------------------------
      if (useHalation) {
        bloomBuffer[0].bind();
        bloomShader.use();
        bindTexture(gl, 0, displayTexture);
        bloomShader.setSampler('SourceTexture', 0);
        bloomShader.set('MaxUV', 1, 1);
        bloomShader.set('SourceSize', signalW, signalH);
        bloomShader.set('Threshold', 0.5);
        quad.draw();

        const blurs = [
          { from: 0, to: 1, dx: 1 / bloomW, dy: 0 },
          { from: 1, to: 2, dx: 0, dy: 1 / bloomH },
        ];
        for (const pass of blurs) {
          bloomBuffer[pass.to].bind();
          blurShader.use();
          bindTexture(gl, 0, bloomBuffer[pass.from].texture);
          blurShader.setSampler('SourceTexture', 0);
          blurShader.set('MaxUV', 1, 1);
          blurShader.set('Direction', pass.dx, pass.dy);
          quad.draw();
        }
      }

      //------------------------------------------------------------------
      // 5. The tube, straight into whatever the host handed us.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      tubeShader.use();
      bindTexture(gl, 0, displayTexture);
      bindTexture(gl, 1, bloomBuffer[2].texture);
      tubeShader.setSampler('SignalTexture', 0);
      tubeShader.setSampler('BloomTexture', 1);
      tubeShader.set('MaxUV', 1, 1);
      tubeShader.set('SignalSize', signalW, signalH);
      tubeShader.set('OutputSize', width, height);

      const maskSpec = MASKS[Math.round(params.get('maskPattern'))];
      tubeShader.set('MaskPattern', Math.round(params.get('maskPattern')));
      tubeShader.set('MaskPitch', lerp(2, 14, params.get('maskPitch')));
      tubeShader.set('MaskStrength', params.get('maskStrength'));
      tubeShader.set('MaskSpill', maskSpec.spill);
      tubeShader.set('MaskGain', maskSpec.gain);

      tubeShader.set('Scanlines', params.get('scanlines'));
      tubeShader.set('BeamBloom', params.get('beamBloom'));
      tubeShader.set('Halation', useHalation ? params.get('halation') * 0.8 : 0);
      tubeShader.set('Brightness', params.get('brightness') * 2);
      tubeShader.set('Contrast', params.get('contrast') * 2);

      tubeShader.set('Curvature', params.get('curvature') * 0.6);
      tubeShader.set('CornerRadius', params.get('cornerRadius') * 0.35);
      // Plus or minus about fifty degrees. Past that the near edge of the
      // screen is closer to the eye than the focal length.
      tubeShader.set('PerspectiveX', (params.get('perspectiveX') - 0.5) * 1.8);
      tubeShader.set('PerspectiveY', (params.get('perspectiveY') - 0.5) * 1.8);
      tubeShader.set('Zoom', lerp(0.5, 1.5, params.get('zoom')));
      tubeShader.set('Vignette', params.get('vignette'));
      quad.draw();

      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);
    },
  };
}

//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;
const unity = (v) => `${(v * 2).toFixed(2)}×`;

mountDemo({
  name: 'Old Cathode',
  pluginId: 'OC01',
  tagline:
    'An analogue television signal path rather than the look of one. It encodes the picture onto a colour subcarrier, damages the composite waveform as a single signal, and decodes it again with no more information than a real receiver had — then paints the result onto a phosphor screen behind curved glass.',
  repo: 'https://github.com/stoatworks-labs/old-cathode',
  page: 'https://stoatworks-labs.com/software/old-cathode/',
  video: 'https://www.youtube.com/watch?v=Dee384n2h5w',

  needFloat: true,
  showBackdrop: true,

  params: [
    // ---- Signal ----------------------------------------------------------
    {
      id: 'system', name: 'System', type: 'option', default: 0, group: 'Signal',
      elements: SYSTEMS.map((s) => s.name),
      display: (v) => `${signalWidth(SYSTEMS[Math.round(v)])}×${signalHeight(SYSTEMS[Math.round(v)])}`,
      hint: 'Everything downstream runs at this standard’s own raster, whatever the composition size is.',
    },
    {
      id: 'source', name: 'Source', type: 'option', default: 0, group: 'Signal',
      elements: SOURCES_SPEC.map((s) => s.name),
      hint: 'Broadcast is full bandwidth. The tape grades each bring their own bandwidths and colour-under delay.',
    },
    { id: 'lumaBandwidth', name: 'Luma Bandwidth', type: 'standard', default: 0.75, group: 'Signal', display: (v) => `${lerp(0.25, 1.25, v).toFixed(2)}× nominal` },
    { id: 'chromaBandwidth', name: 'Chroma Bandwidth', type: 'standard', default: 0.75, group: 'Signal', display: (v) => `${lerp(0.25, 1.25, v).toFixed(2)}× nominal` },
    { id: 'saturation', name: 'Saturation', type: 'standard', default: 0.5, group: 'Signal', display: unity },
    {
      id: 'tint', name: 'Tint', type: 'standard', default: 0.5, group: 'Signal',
      display: (v) => `${(((v - 0.5) * 1.4 * 180) / Math.PI).toFixed(0)}°`,
      hint: 'Phase error in the receiver’s local reference. It rotates every hue in NTSC — and PAL’s delay line trades that for a little saturation instead.',
    },
    {
      id: 'dotCrawl', name: 'Dot Crawl', type: 'standard', default: 0.6, group: 'Signal',
      display: pct,
      hint: 'How much subcarrier is left in the luminance. Turning it down notches the carrier out and loses the fine detail with it — the trade the real sets made.',
    },
    { id: 'ghosting', name: 'Ghosting', type: 'standard', default: 0.0, group: 'Signal', display: pct },
    { id: 'ghostDelay', name: 'Ghost Delay', type: 'standard', default: 0.3, group: 'Signal', display: (v) => `${(v * 48).toFixed(0)} samples` },
    { id: 'noise', name: 'Noise', type: 'standard', default: 0.12, group: 'Signal', display: pct },
    { id: 'dropouts', name: 'Dropouts', type: 'standard', default: 0.0, group: 'Signal', display: pct },
    { id: 'interference', name: 'Interference', type: 'standard', default: 0.0, group: 'Signal', display: pct },

    // ---- Sync ------------------------------------------------------------
    { id: 'verticalHold', name: 'Vertical Hold', type: 'standard', default: 0.0, group: 'Sync', display: pct },
    { id: 'jitter', name: 'Jitter', type: 'standard', default: 0.08, group: 'Sync', display: pct },
    { id: 'tracking', name: 'Tracking', type: 'standard', default: 0.0, group: 'Sync', display: pct },
    { id: 'headSwitch', name: 'Head Switch', type: 'standard', default: 0.0, group: 'Sync', display: pct },
    { id: 'hum', name: 'Hum', type: 'standard', default: 0.0, group: 'Sync', display: pct },
    { id: 'interlace', name: 'Interlace', type: 'boolean', default: 0, group: 'Sync' },

    // ---- Tube ------------------------------------------------------------
    {
      id: 'maskPattern', name: 'Mask Pattern', type: 'option', default: 1, group: 'Tube',
      elements: MASKS.map((m) => m.name),
      hint: 'The phosphor layout, living on the glass — so it foreshortens with the tube rather than sliding about over the picture.',
    },
    { id: 'maskPitch', name: 'Mask Pitch', type: 'standard', default: 0.35, group: 'Tube', display: (v) => `${lerp(2, 14, v).toFixed(1)} px` },
    { id: 'maskStrength', name: 'Mask Strength', type: 'standard', default: 0.6, group: 'Tube', display: pct },
    { id: 'scanlines', name: 'Scanlines', type: 'standard', default: 0.5, group: 'Tube', display: pct },
    { id: 'beamBloom', name: 'Beam Bloom', type: 'standard', default: 0.5, group: 'Tube', display: pct },
    { id: 'persistence', name: 'Persistence', type: 'standard', default: 0.15, group: 'Tube', display: pct },
    { id: 'halation', name: 'Halation', type: 'standard', default: 0.25, group: 'Tube', display: pct },
    { id: 'brightness', name: 'Brightness', type: 'standard', default: 0.5, group: 'Tube', display: unity },
    { id: 'contrast', name: 'Contrast', type: 'standard', default: 0.5, group: 'Tube', display: unity },

    // ---- Geometry --------------------------------------------------------
    { id: 'curvature', name: 'Curvature', type: 'standard', default: 0.25, group: 'Geometry', display: pct },
    { id: 'cornerRadius', name: 'Corner Radius', type: 'standard', default: 0.15, group: 'Geometry', display: pct },
    { id: 'perspectiveX', name: 'Perspective X', type: 'standard', default: 0.5, group: 'Geometry', display: (v) => `${(((v - 0.5) * 1.8 * 180) / Math.PI).toFixed(0)}°` },
    { id: 'perspectiveY', name: 'Perspective Y', type: 'standard', default: 0.5, group: 'Geometry', display: (v) => `${(((v - 0.5) * 1.8 * 180) / Math.PI).toFixed(0)}°` },
    { id: 'zoom', name: 'Zoom', type: 'standard', default: 0.5, group: 'Geometry', display: (v) => `${lerp(0.5, 1.5, v).toFixed(2)}×` },
    { id: 'vignette', name: 'Vignette', type: 'standard', default: 0.35, group: 'Geometry', display: pct },
  ],

  sources: ['scene', 'detail', 'bars', 'grid', 'ramp', 'spot'],

  presets: {
    'Broadcast NTSC': {},
    'PAL, clean': { system: 1, noise: 0.04, jitter: 0.03, dotCrawl: 0.35 },
    'VHS EP, worn tape': {
      source: 3, noise: 0.4, dropouts: 0.5, tracking: 0.35, headSwitch: 0.6,
      jitter: 0.3, chromaBandwidth: 0.4, persistence: 0.3,
    },
    'Weak aerial': { noise: 0.55, ghosting: 0.5, ghostDelay: 0.4, interference: 0.35, saturation: 0.35 },
    'Rolling picture': { verticalHold: 0.35, jitter: 0.2, hum: 0.4 },
    'Trinitron, off-axis': {
      maskPattern: 2, maskPitch: 0.5, maskStrength: 0.85, perspectiveX: 0.68,
      curvature: 0.1, cornerRadius: 0.06, noise: 0.05,
    },
    'Mask, close up': { maskPattern: 1, maskPitch: 0.9, maskStrength: 1, scanlines: 0.8, zoom: 0.5 },
  },

  differences: [
    'The plugin asks the host for its clock (SetTimeSupported) so that re-rendering a composition gives the same snow rather than whatever the wall clock said. Here the clock is the page’s own, accumulated from frame deltas — which is why Restart puts the noise back to where it started.',
    'Interlace and Persistence both depend on frame-to-frame history, so they behave differently at a browser’s frame rate than at a composition’s. Step advances exactly one frame if you want to see the field sequence.',
    'The signal chain runs in 16-bit float here as it does in the plugin, which needs EXT_color_buffer_float. If your browser lacks it the page says so rather than quietly dropping to 8 bits and banding the decode.',
  ],

  createRenderer,
});
