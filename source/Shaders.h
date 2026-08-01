#pragma once

/**
    The GLSL for each stage of the chain.

    Old Cathode is not a stack of overlays. It is a model of the path a picture
    actually took to reach a television: RGB is encoded onto a colour subcarrier,
    the whole composite signal is degraded as one, and only then is it decoded
    again and painted onto a phosphor screen. Everything that looks like a
    separate "effect" -- dot crawl, cross-colour, chroma smear, snow -- falls out
    of doing those two conversions imperfectly, which is what the real hardware
    did.

    The stages, in signal order:

      Resample   full-resolution RGB -> the SD raster the standard defines,
                 band-limited first so nothing aliases onto the subcarrier.
      Signal     encode to composite, damage it, decode it back.
      Phosphor   emissive decay, so the picture does not vanish the instant the
                 beam leaves it.
      Bloom      halation: light scattered sideways inside the glass faceplate.
      Tube       the screen itself -- shadow mask, scanning beam, curvature,
                 and the fact that you are looking at it from somewhere.

    Each is a separate program because each runs at a different resolution. The
    signal stages run at SD (754x486 or 921x576) no matter what Resolume's
    composition size is, which is both authentic and the reason the expensive
    filtering is affordable.
*/
namespace oldcathode::shaders
{
/// Shared by every pass: draws the screen quad and scales UVs by MaxUV so the
/// same program works against a host texture with padding and against our own
/// framebuffers, which have none.
extern const char* const kVertex;

extern const char* const kResampleFragment;
extern const char* const kSignalFragment;
extern const char* const kPhosphorFragment;
extern const char* const kBloomFragment;
extern const char* const kBlurFragment;
extern const char* const kTubeFragment;

} // namespace oldcathode::shaders
