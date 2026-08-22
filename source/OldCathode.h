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
	char* GetTextParameter( unsigned int index ) override;

	/// Load-bearing, and its absence is invisible offline.
	///
	/// instantiateGL pushes the declared default of every FF_TYPE_TEXT and
	/// FF_TYPE_FILE parameter back through this on a fresh instance, and
	/// deletes the instance the moment one returns FF_FAIL -- which is exactly
	/// what CFFGLPlugin's stub does. Declaring the About line without this
	/// override therefore made the plugin impossible to create in any real
	/// host, while every harness in this repo, which drives the class
	/// directly, carried on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param
	/// order. Handed out rather than copied into the harness, so a second
	/// list cannot go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

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
	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything -- in which case it must not
	/// reach params[] and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	void applyPreset( int presetIndex );

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// applyPreset raises -- Resolume does not. So a preset that writes params[]
	/// and trusts the host to follow is relying on behaviour the specification
	/// never promised, and when the host instead restates the values it still
	/// believes in, the rule that a covered parameter changing means the
	/// operator has taken over fires on the host's own echo and drops straight
	/// back to Custom. Reported against vertigo as its issue #2; the same
	/// pattern had been copied into all seven plugins.
	///
	/// Keeping the host's own last word separately is what tells the two apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	bool compileShaders();
	void releaseBuffers();

	/// Seconds to drive the drifting impairments with. The host's timeline when
	/// there is one, so a re-render is reproducible; the wall clock when there is
	/// not, so the picture is not frozen in a host that never calls SetTime.
	float elapsedSeconds();

public:
	/// Clock test hooks. The harness DECLARES its unit rather than leaving
	/// elapsedSeconds to infer one -- an absolute time in a single frame is
	/// genuinely ambiguous, and an implicit unit is what let a thousand-times-
	/// fast bug sit here unnoticed.
	void SetClockScaleForTest( double scale );
	double ClockScaleForTest() const;

private:


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

	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen = false;
	std::chrono::steady_clock::time_point startTime;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
