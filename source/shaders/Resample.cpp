#include "../Shaders.h"

namespace oldcathode::shaders
{
/// Full-resolution RGB down to the raster the broadcast standard defines.
///
/// This exists because of what the next stage does. Dropping 1080p or 2160p
/// straight onto a 754-sample line by point sampling would alias, and the
/// aliased energy would then be modulated onto the colour subcarrier and come
/// back out of the decoder as cross-colour -- rainbows on detail that never had
/// any colour in it. Real chains do produce that artefact, but only from detail
/// that genuinely sat near the subcarrier frequency, because everything above
/// the channel bandwidth was filtered off before it was ever sampled.
///
/// So this is that filter: a box the width of one destination sample. Without
/// it the effect's most characteristic artefact would be driven by the
/// composition resolution instead of by the signal, and would crawl differently
/// every time somebody changed their output size.
const char* const kResampleFragment = R"(#version 410 core
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
)";
} // namespace oldcathode::shaders
