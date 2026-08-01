#include "../Shaders.h"

namespace oldcathode::shaders
{
/// Halation: the bright-pass and quarter-resolution downsample.
///
/// The glass faceplate of a tube is thick, and light leaving a phosphor grain
/// scatters sideways inside it before it gets out. On a dim picture you never
/// notice; on a bright one it is why highlights swell and why the black next to
/// a white caption is never quite black.
///
/// Scattering is proportional to the light there is, so this is not really a
/// threshold effect -- but the part below the knee is invisible against the
/// picture itself, and skipping it keeps the blur from washing out midtones
/// that should stay put.
const char* const kBloomFragment = R"(#version 410 core
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
)";

/// The scatter itself, run twice: once across, once down.
///
/// A Gaussian is the right shape here for the ordinary reason -- repeated
/// scattering events converge on one -- and separating it into two passes is
/// what makes a wide halo affordable. The taps are placed between texels so
/// each bilinear fetch does the work of two.
const char* const kBlurFragment = R"(#version 410 core
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
)";
} // namespace oldcathode::shaders
