#include "../Shaders.h"

namespace oldcathode::shaders
{
/// Emissive decay.
///
/// A phosphor is not a pixel. Nothing holds its value: it is struck once per
/// field and then falls off on its own, so what the eye sees is the sum of a
/// fresh line and the fading remains of everything painted before it. That is
/// why bright objects trail on a CRT and why an interlaced picture does not
/// simply flicker to black on the lines the current field is not refreshing.
///
/// It decays rather than averages -- max() against a decayed history, not a
/// lerp -- because light adds. A lerp would darken a new bright pixel that
/// happens to land where a dim one was, which is backwards.
///
/// The three phosphors of a P22 screen do not decay at the same rate. Blue goes
/// first, green hangs on longest, so a white object dragged across a real tube
/// leaves a faintly green wake. That is one multiply, and it is the detail that
/// makes the trail look like a tube rather than like a feedback buffer.
const char* const kPhosphorFragment = R"(#version 410 core
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
)";
} // namespace oldcathode::shaders
