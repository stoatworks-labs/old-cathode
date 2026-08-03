#pragma once

#include <FFGLSDK.h>

#include <chrono>

#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    Old Cathode -- an analogue television signal path for Resolume.

    The effect is a model of a route, not a set of filters. A picture is encoded
    onto a colour subcarrier the way a broadcast encoder would have done it, the
    resulting composite waveform is degraded as a single signal, and a
    synchronous demodulator takes it apart again with no more information than a
    real receiver had. It is then painted onto a phosphor screen with a mask, a
    beam that defocuses when it is bright, a curved face, and a viewer sitting
    somewhere in particular.

    Almost everything an operator recognises as "the CRT look" is a consequence
    of that path rather than a feature of this plugin. Dot crawl, cross-colour,
    smeared chroma and coloured snow are all things the decoder does when it is
    handed a signal it cannot fully disentangle -- so they arrive on their own,
    correlated with each other in the way the real artefacts are.

    See Shaders.h for the stages and AGENTS.md for the traps.
*/
class OldCathode : public CFFGLPlugin
{
public:
	OldCathode();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

private:
	/// Everything the operator can reach. The order is the order Resolume shows
	/// them in, and it is the order the signal travels: what was transmitted,
	/// what the timebase did to it, what the tube did with it, and where you are
	/// sitting.
	enum ParamID : FFUInt32
	{
		//Signal
		PT_SYSTEM,
		PT_SOURCE,
		PT_LUMA_BANDWIDTH,
		PT_CHROMA_BANDWIDTH,
		PT_SATURATION,
		PT_TINT,
		PT_DOT_CRAWL,
		PT_GHOSTING,
		PT_GHOST_DELAY,
		PT_NOISE,
		PT_DROPOUTS,
		PT_INTERFERENCE,

		//Sync
		PT_VERTICAL_HOLD,
		PT_JITTER,
		PT_TRACKING,
		PT_HEAD_SWITCH,
		PT_HUM,
		PT_INTERLACE,

		//Tube
		PT_MASK_PATTERN,
		PT_MASK_PITCH,
		PT_MASK_STRENGTH,
		PT_SCANLINES,
		PT_BEAM_BLOOM,
		PT_PERSISTENCE,
		PT_HALATION,
		PT_BRIGHTNESS,
		PT_CONTRAST,

		//Geometry
		PT_CURVATURE,
		PT_CORNER_RADIUS,
		PT_PERSPECTIVE_X,
		PT_PERSPECTIVE_Y,
		PT_ZOOM,
		PT_VIGNETTE,

		//Preset. Declared after the real controls so their IDs — which a saved
		//composition refers to — do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ oldcathode::presets::kParamCount ] = {
		PT_SYSTEM, PT_SOURCE, PT_LUMA_BANDWIDTH, PT_CHROMA_BANDWIDTH, PT_SATURATION, PT_TINT,
		PT_DOT_CRAWL, PT_GHOSTING, PT_GHOST_DELAY, PT_NOISE, PT_DROPOUTS, PT_INTERFERENCE,
		PT_VERTICAL_HOLD, PT_JITTER, PT_TRACKING, PT_HEAD_SWITCH, PT_HUM, PT_INTERLACE,
		PT_MASK_PATTERN, PT_MASK_PITCH, PT_MASK_STRENGTH, PT_SCANLINES, PT_BEAM_BLOOM,
		PT_PERSISTENCE, PT_HALATION, PT_BRIGHTNESS, PT_CONTRAST, PT_CURVATURE,
		PT_CORNER_RADIUS, PT_VIGNETTE
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	bool compileShaders();
	void releaseBuffers();

	/// Seconds to drive the drifting impairments with. The host's timeline when
	/// there is one, so a re-render is reproducible; the wall clock when there is
	/// not, so the picture is not frozen in a host that never calls SetTime.
	float elapsedSeconds() const;

	ffglex::FFGLShader resampleShader;
	ffglex::FFGLShader signalShader;
	ffglex::FFGLShader phosphorShader;
	ffglex::FFGLShader bloomShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader tubeShader;
	ffglex::FFGLScreenQuad quad;

	oldcathode::PassBuffer resampleBuffer;//the picture on the standard's raster
	oldcathode::PassBuffer signalBuffer;  //...after the round trip through composite
	oldcathode::PassBuffer phosphorBuffer[ 2 ];
	oldcathode::PassBuffer bloomBuffer[ 3 ];

	int phosphorIndex = 0;  //!< Which half of the ping-pong this frame writes to.
	float frameIndex = 0.0f;//!< Drives the subcarrier's frame-to-frame phase walk.

	bool hostTimeSeen = false;
	std::chrono::steady_clock::time_point startTime;

	float params[ PT_COUNT ];

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
