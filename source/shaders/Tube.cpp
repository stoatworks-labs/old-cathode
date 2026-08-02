#include "../Shaders.h"

namespace oldcathode::shaders
{
/// The screen: where the decoded signal stops being a signal and becomes light
/// coming off a sheet of glass that you are looking at from somewhere.
///
/// The order of operations here is the physical one, and it matters:
///
///   1. Undo the view. Work out which point on the tube's face this output
///      pixel is looking at, by intersecting the eye ray with the plane of the
///      screen. Everything after this is in the tube's own coordinates.
///   2. Undo the curvature. The face is not flat, so the picture painted on it
///      bulges; sampling has to bulge the opposite way.
///   3. Scan. The beam is a spot with a profile, not a row of squares, and it
///      is fatter where it is brighter because more current defocuses it.
///   4. Mask. The phosphors are physically on the glass, so the mask lives in
///      tube coordinates -- it turns with the tube when you look at it from an
///      angle, and it does not slide about when the picture moves.
///
/// Doing the mask in tube space rather than output space is the whole reason
/// the perspective control looks like a television and not like a picture of
/// one with a screen door in front of it.
const char* const kTubeFragment = R"(#version 410 core
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
//
// Only the shape lives here. The spill floor and the gain that makes up the
// light the mask costs are uniforms, because they are measured constants and
// they belong beside the rest of the physical figures rather than buried in a
// return value.
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
	// behind it. It is not cosmetic here. Without it the distortion pulls the
	// picture's own corners inside the glass and shows black beyond them -- the
	// one thing a correctly set-up television never does -- and the shape you
	// see at the edge of the screen becomes an artefact of the curvature
	// instead of the shape of the tube, which leaves Corner Radius doing
	// nothing at any useful curvature.
	//
	// The coupling to Curvature is real: more distortion means more overscan is
	// needed to cover the same face. Zoom is separate and stays separate -- that
	// moves the set relative to the viewer, and takes the tube with it.
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
	//fades out. The threshold matters more than it looks: 486 lines into a
	//1080-line composition is 2.2 pixels each, which is the single most common
	//case there is, and it has to be on the full-strength side of this.
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
	//The worst axis, not the length of the pair -- the two are independent here
	//and combining them would overstate the rate by root two, fading the mask
	//out while it is still perfectly resolvable.
	vec2 maskRate = fwidth( maskCoord );
	float dotsPerPixel = max( maskRate.x, maskRate.y );
	float maskAA = 1.0 - smoothstep( 0.4, 0.8, dotsPerPixel );

	//The spot is wider than one phosphor, so its neighbours are always partly
	//lit. That floor is why a real mask reads as a texture over the picture
	//rather than as three separated primaries, and it is what keeps the gain
	//needed to restore the light small enough not to clip every highlight.
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
)";
} // namespace oldcathode::shaders
