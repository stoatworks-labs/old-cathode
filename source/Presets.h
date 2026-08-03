#pragma once

/**
    Factory presets: named situations an operator can reach in one gesture.

    Each entry is a *place the signal has been* — a studio monitor fed by
    cable, a rooftop aerial at the edge of reception, a rental tape on its
    fourth owner — not a random collection of slider positions. The controls
    this plugin exposes are stages of a signal path, so a coherent look is a
    coherent story about that path, and these are the stories people actually
    reach for.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Plain data only; the
    application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it means "the sliders are the truth".

    A preset covers the signal, sync, tube and screen-shape parameters.
    Perspective and Zoom are framing — where the viewer sits is the
    operator's business, not the preset's.
*/

namespace oldcathode
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kSystem,
	kSource,
	kLumaBandwidth,
	kChromaBandwidth,
	kSaturation,
	kTint,
	kDotCrawl,
	kGhosting,
	kGhostDelay,
	kNoise,
	kDropouts,
	kInterference,
	kVerticalHold,
	kJitter,
	kTracking,
	kHeadSwitch,
	kHum,
	kInterlace,
	kMaskPattern,
	kMaskPitch,
	kMaskStrength,
	kScanlines,
	kBeamBloom,
	kPersistence,
	kHalation,
	kBrightness,
	kContrast,
	kCurvature,
	kCornerRadius,
	kVignette,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices: System 0 NTSC / 1 PAL; Source 0
// Broadcast / 1 VHS SP / 2 VHS LP / 3 VHS EP; Mask 0 None / 1 Shadow Mask /
// 2 Aperture Grille / 3 Slot Mask / 4 RGB Stripe. Saturation, Tint,
// Brightness and Contrast sit at unity on 0.5.
inline constexpr Preset kPresets[] = {
	// A production monitor fed by cable: the standard at its best behaviour.
	// Sharp, stable, a grille tube with barely any face to it.
	{ "Studio Monitor",
	  { /*Sys*/ 0, /*Src*/ 0, /*LumaBw*/ 1.0f, /*ChromaBw*/ 1.0f, /*Sat*/ 0.5f, /*Tint*/ 0.5f,
	    /*Crawl*/ 0.3f, /*Ghost*/ 0.0f, /*GDelay*/ 0.3f, /*Noise*/ 0.02f, /*Drop*/ 0.0f, /*Intf*/ 0.0f,
	    /*VHold*/ 0.0f, /*Jitter*/ 0.02f, /*Track*/ 0.0f, /*Head*/ 0.0f, /*Hum*/ 0.0f, /*Ilace*/ 0.0f,
	    /*Mask*/ 2, /*Pitch*/ 0.25f, /*MaskStr*/ 0.5f, /*Scan*/ 0.6f, /*Bloom*/ 0.3f, /*Persist*/ 0.1f,
	    /*Halation*/ 0.15f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f, /*Curve*/ 0.05f, /*Corner*/ 0.1f, /*Vig*/ 0.15f } },

	// A living-room set on an aerial: decent signal, a multipath ghost off
	// the hills, a little snow, a shadow-mask tube with some curve to it.
	{ "Rooftop Aerial",
	  { /*Sys*/ 0, /*Src*/ 0, /*LumaBw*/ 0.75f, /*ChromaBw*/ 0.75f, /*Sat*/ 0.5f, /*Tint*/ 0.5f,
	    /*Crawl*/ 0.6f, /*Ghost*/ 0.45f, /*GDelay*/ 0.35f, /*Noise*/ 0.3f, /*Drop*/ 0.0f, /*Intf*/ 0.15f,
	    /*VHold*/ 0.0f, /*Jitter*/ 0.08f, /*Track*/ 0.0f, /*Head*/ 0.0f, /*Hum*/ 0.05f, /*Ilace*/ 0.0f,
	    /*Mask*/ 1, /*Pitch*/ 0.35f, /*MaskStr*/ 0.6f, /*Scan*/ 0.5f, /*Bloom*/ 0.5f, /*Persist*/ 0.15f,
	    /*Halation*/ 0.25f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f, /*Curve*/ 0.3f, /*Corner*/ 0.18f, /*Vig*/ 0.35f } },

	// The same set, after midnight, on the fringe of the coverage area.
	{ "Late-Night UHF",
	  { /*Sys*/ 0, /*Src*/ 0, /*LumaBw*/ 0.6f, /*ChromaBw*/ 0.55f, /*Sat*/ 0.45f, /*Tint*/ 0.47f,
	    /*Crawl*/ 0.7f, /*Ghost*/ 0.3f, /*GDelay*/ 0.5f, /*Noise*/ 0.55f, /*Drop*/ 0.0f, /*Intf*/ 0.4f,
	    /*VHold*/ 0.05f, /*Jitter*/ 0.15f, /*Track*/ 0.0f, /*Head*/ 0.0f, /*Hum*/ 0.2f, /*Ilace*/ 0.0f,
	    /*Mask*/ 1, /*Pitch*/ 0.4f, /*MaskStr*/ 0.6f, /*Scan*/ 0.5f, /*Bloom*/ 0.55f, /*Persist*/ 0.2f,
	    /*Halation*/ 0.3f, /*Bright*/ 0.48f, /*Contrast*/ 0.47f, /*Curve*/ 0.3f, /*Corner*/ 0.18f, /*Vig*/ 0.45f } },

	// A tape recorded at standard play and looked after: the colour-under
	// softness and the head-switch flag, but nothing failing yet.
	{ "VHS, Good Copy",
	  { /*Sys*/ 0, /*Src*/ 1, /*LumaBw*/ 0.75f, /*ChromaBw*/ 0.75f, /*Sat*/ 0.5f, /*Tint*/ 0.5f,
	    /*Crawl*/ 0.5f, /*Ghost*/ 0.0f, /*GDelay*/ 0.3f, /*Noise*/ 0.15f, /*Drop*/ 0.1f, /*Intf*/ 0.0f,
	    /*VHold*/ 0.0f, /*Jitter*/ 0.12f, /*Track*/ 0.05f, /*Head*/ 0.3f, /*Hum*/ 0.0f, /*Ilace*/ 0.0f,
	    /*Mask*/ 1, /*Pitch*/ 0.35f, /*MaskStr*/ 0.55f, /*Scan*/ 0.5f, /*Bloom*/ 0.5f, /*Persist*/ 0.15f,
	    /*Halation*/ 0.25f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f, /*Curve*/ 0.28f, /*Corner*/ 0.16f, /*Vig*/ 0.3f } },

	// Six-hour mode, fourth owner, stored in a loft: tracking noise, dropouts,
	// a wandering vertical hold. The look everyone means by "VHS".
	{ "Rental Tape",
	  { /*Sys*/ 0, /*Src*/ 3, /*LumaBw*/ 0.6f, /*ChromaBw*/ 0.5f, /*Sat*/ 0.55f, /*Tint*/ 0.48f,
	    /*Crawl*/ 0.6f, /*Ghost*/ 0.1f, /*GDelay*/ 0.3f, /*Noise*/ 0.3f, /*Drop*/ 0.55f, /*Intf*/ 0.05f,
	    /*VHold*/ 0.12f, /*Jitter*/ 0.35f, /*Track*/ 0.5f, /*Head*/ 0.7f, /*Hum*/ 0.05f, /*Ilace*/ 0.0f,
	    /*Mask*/ 1, /*Pitch*/ 0.35f, /*MaskStr*/ 0.6f, /*Scan*/ 0.5f, /*Bloom*/ 0.55f, /*Persist*/ 0.2f,
	    /*Halation*/ 0.3f, /*Bright*/ 0.5f, /*Contrast*/ 0.48f, /*Curve*/ 0.28f, /*Corner*/ 0.16f, /*Vig*/ 0.35f } },

	// The 5.5MHz system on a broadcast feed: sharper luma than NTSC, the
	// slower field rate's heavier flicker left to the interlace control.
	{ "PAL Broadcast",
	  { /*Sys*/ 1, /*Src*/ 0, /*LumaBw*/ 0.8f, /*ChromaBw*/ 0.75f, /*Sat*/ 0.5f, /*Tint*/ 0.5f,
	    /*Crawl*/ 0.5f, /*Ghost*/ 0.1f, /*GDelay*/ 0.3f, /*Noise*/ 0.12f, /*Drop*/ 0.0f, /*Intf*/ 0.0f,
	    /*VHold*/ 0.0f, /*Jitter*/ 0.06f, /*Track*/ 0.0f, /*Head*/ 0.0f, /*Hum*/ 0.05f, /*Ilace*/ 0.0f,
	    /*Mask*/ 1, /*Pitch*/ 0.35f, /*MaskStr*/ 0.6f, /*Scan*/ 0.55f, /*Bloom*/ 0.5f, /*Persist*/ 0.15f,
	    /*Halation*/ 0.25f, /*Bright*/ 0.5f, /*Contrast*/ 0.5f, /*Curve*/ 0.28f, /*Corner*/ 0.16f, /*Vig*/ 0.32f } },

	// RGB into a slot-mask tube in a smoky room: no composite artefacts to
	// speak of, everything is the tube — scanlines, bloom, curvature.
	{ "Arcade Cabinet",
	  { /*Sys*/ 0, /*Src*/ 0, /*LumaBw*/ 1.0f, /*ChromaBw*/ 1.0f, /*Sat*/ 0.6f, /*Tint*/ 0.5f,
	    /*Crawl*/ 0.1f, /*Ghost*/ 0.0f, /*GDelay*/ 0.3f, /*Noise*/ 0.02f, /*Drop*/ 0.0f, /*Intf*/ 0.0f,
	    /*VHold*/ 0.0f, /*Jitter*/ 0.03f, /*Track*/ 0.0f, /*Head*/ 0.0f, /*Hum*/ 0.0f, /*Ilace*/ 0.0f,
	    /*Mask*/ 3, /*Pitch*/ 0.45f, /*MaskStr*/ 0.75f, /*Scan*/ 0.8f, /*Bloom*/ 0.7f, /*Persist*/ 0.2f,
	    /*Halation*/ 0.35f, /*Bright*/ 0.52f, /*Contrast*/ 0.55f, /*Curve*/ 0.35f, /*Corner*/ 0.2f, /*Vig*/ 0.4f } },

	// A tube at the end of its emission: everything blooms, the image lags
	// behind itself, mains hum has found its way in, and the corners have
	// gone dark. The set still works; nobody would call it well.
	{ "Dying Tube",
	  { /*Sys*/ 0, /*Src*/ 0, /*LumaBw*/ 0.65f, /*ChromaBw*/ 0.6f, /*Sat*/ 0.42f, /*Tint*/ 0.46f,
	    /*Crawl*/ 0.6f, /*Ghost*/ 0.1f, /*GDelay*/ 0.3f, /*Noise*/ 0.2f, /*Drop*/ 0.0f, /*Intf*/ 0.1f,
	    /*VHold*/ 0.2f, /*Jitter*/ 0.2f, /*Track*/ 0.0f, /*Head*/ 0.0f, /*Hum*/ 0.4f, /*Ilace*/ 0.0f,
	    /*Mask*/ 1, /*Pitch*/ 0.4f, /*MaskStr*/ 0.65f, /*Scan*/ 0.45f, /*Bloom*/ 0.8f, /*Persist*/ 0.5f,
	    /*Halation*/ 0.6f, /*Bright*/ 0.45f, /*Contrast*/ 0.4f, /*Curve*/ 0.4f, /*Corner*/ 0.22f, /*Vig*/ 0.55f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace oldcathode
