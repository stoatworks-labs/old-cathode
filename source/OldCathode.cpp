#include "OldCathode.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Shaders.h"
#include "Standards.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace oldcathode;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< OldCathode >,                          // Create method
	"OC01",                                               // Plugin unique ID of maximum length 4.
	"Old Cathode",                                        // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	1,                                                    // Plugin major version number
	0,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"Analogue television signal path and CRT display",    // Plugin description
	"Old Cathode FFGL effect"                             // About
);

namespace
{
float lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

OldCathode::OldCathode() :
	startTime( std::chrono::steady_clock::now() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The impairments drift, so the effect needs a clock. Resolume provides one;
	//asking for it means a re-render of the same composition produces the same
	//snow rather than whatever the wall clock happened to say.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They are set to a plausible television rather than to nothing: a
	// broadcast NTSC picture on a shadow-mask tube with a little noise on it.
	// An effect that does nothing until eight sliders are moved is an effect
	// nobody finds out is any good.
	//---------------------------------------------------------------------
	params[ PT_SYSTEM ]           = 0.0f;
	params[ PT_SOURCE ]           = 0.0f;
	params[ PT_LUMA_BANDWIDTH ]   = 0.75f;//0.75 is exactly what the standard specifies
	params[ PT_CHROMA_BANDWIDTH ] = 0.75f;
	params[ PT_SATURATION ]       = 0.5f; //0.5 is unity
	params[ PT_TINT ]             = 0.5f; //0.5 is no phase error
	params[ PT_DOT_CRAWL ]        = 0.6f;
	params[ PT_GHOSTING ]         = 0.0f;
	params[ PT_GHOST_DELAY ]      = 0.3f;
	params[ PT_NOISE ]            = 0.12f;
	params[ PT_DROPOUTS ]         = 0.0f;
	params[ PT_INTERFERENCE ]     = 0.0f;

	params[ PT_VERTICAL_HOLD ]    = 0.0f;
	params[ PT_JITTER ]           = 0.08f;
	params[ PT_TRACKING ]         = 0.0f;
	params[ PT_HEAD_SWITCH ]      = 0.0f;
	params[ PT_HUM ]              = 0.0f;
	params[ PT_INTERLACE ]        = 0.0f;

	params[ PT_MASK_PATTERN ]     = 1.0f;//shadow mask
	params[ PT_MASK_PITCH ]       = 0.35f;
	params[ PT_MASK_STRENGTH ]    = 0.6f;
	params[ PT_SCANLINES ]        = 0.5f;
	params[ PT_BEAM_BLOOM ]       = 0.5f;
	params[ PT_PERSISTENCE ]      = 0.15f;
	params[ PT_HALATION ]         = 0.25f;
	params[ PT_BRIGHTNESS ]       = 0.5f;//0.5 is unity
	params[ PT_CONTRAST ]         = 0.5f;//0.5 is unity

	params[ PT_CURVATURE ]        = 0.25f;
	params[ PT_CORNER_RADIUS ]    = 0.15f;
	params[ PT_PERSPECTIVE_X ]    = 0.5f;//0.5 is straight on
	params[ PT_PERSPECTIVE_Y ]    = 0.5f;
	params[ PT_ZOOM ]             = 0.5f;//0.5 is 1:1
	params[ PT_VIGNETTE ]         = 0.35f;

	//---------------------------------------------------------------------
	// Declaration. The groups matter: this is thirty-odd parameters, and an
	// ungrouped list of thirty in somebody else's inspector is unusable.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_SYSTEM, "System", systemCount(), params[ PT_SYSTEM ] );
	for( int i = 0; i < systemCount(); ++i )
		SetParamElementInfo( PT_SYSTEM, i, system( i ).name, static_cast< float >( i ) );

	SetOptionParamInfo( PT_SOURCE, "Source", sourceCount(), params[ PT_SOURCE ] );
	for( int i = 0; i < sourceCount(); ++i )
		SetParamElementInfo( PT_SOURCE, i, source( i ).name, static_cast< float >( i ) );

	SetParamInfof( PT_LUMA_BANDWIDTH, "Luma Bandwidth", FF_TYPE_STANDARD );
	SetParamInfof( PT_CHROMA_BANDWIDTH, "Chroma Bandwidth", FF_TYPE_STANDARD );
	SetParamInfof( PT_SATURATION, "Saturation", FF_TYPE_STANDARD );
	SetParamInfof( PT_TINT, "Tint", FF_TYPE_STANDARD );
	SetParamInfof( PT_DOT_CRAWL, "Dot Crawl", FF_TYPE_STANDARD );
	SetParamInfof( PT_GHOSTING, "Ghosting", FF_TYPE_STANDARD );
	SetParamInfof( PT_GHOST_DELAY, "Ghost Delay", FF_TYPE_STANDARD );
	SetParamInfof( PT_NOISE, "Noise", FF_TYPE_STANDARD );
	SetParamInfof( PT_DROPOUTS, "Dropouts", FF_TYPE_STANDARD );
	SetParamInfof( PT_INTERFERENCE, "Interference", FF_TYPE_STANDARD );

	SetParamInfof( PT_VERTICAL_HOLD, "Vertical Hold", FF_TYPE_STANDARD );
	SetParamInfof( PT_JITTER, "Jitter", FF_TYPE_STANDARD );
	SetParamInfof( PT_TRACKING, "Tracking", FF_TYPE_STANDARD );
	SetParamInfof( PT_HEAD_SWITCH, "Head Switch", FF_TYPE_STANDARD );
	SetParamInfof( PT_HUM, "Hum", FF_TYPE_STANDARD );
	SetParamInfof( PT_INTERLACE, "Interlace", FF_TYPE_BOOLEAN );

	SetOptionParamInfo( PT_MASK_PATTERN, "Mask Pattern", maskCount(), params[ PT_MASK_PATTERN ] );
	for( int i = 0; i < maskCount(); ++i )
		SetParamElementInfo( PT_MASK_PATTERN, i, mask( i ).name, static_cast< float >( i ) );

	SetParamInfof( PT_MASK_PITCH, "Mask Pitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_MASK_STRENGTH, "Mask Strength", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCANLINES, "Scanlines", FF_TYPE_STANDARD );
	SetParamInfof( PT_BEAM_BLOOM, "Beam Bloom", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSISTENCE, "Persistence", FF_TYPE_STANDARD );
	SetParamInfof( PT_HALATION, "Halation", FF_TYPE_STANDARD );
	SetParamInfof( PT_BRIGHTNESS, "Brightness", FF_TYPE_STANDARD );
	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );

	SetParamInfof( PT_CURVATURE, "Curvature", FF_TYPE_STANDARD );
	SetParamInfof( PT_CORNER_RADIUS, "Corner Radius", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSPECTIVE_X, "Perspective X", FF_TYPE_STANDARD );
	SetParamInfof( PT_PERSPECTIVE_Y, "Perspective Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ZOOM, "Zoom", FF_TYPE_STANDARD );
	SetParamInfof( PT_VIGNETTE, "Vignette", FF_TYPE_STANDARD );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, "" );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_SYSTEM; i <= PT_INTERFERENCE; ++i )
		SetParamGroup( i, "Signal" );
	for( FFUInt32 i = PT_VERTICAL_HOLD; i <= PT_INTERLACE; ++i )
		SetParamGroup( i, "Sync" );
	for( FFUInt32 i = PT_MASK_PATTERN; i <= PT_CONTRAST; ++i )
		SetParamGroup( i, "Tube" );
	for( FFUInt32 i = PT_CURVATURE; i <= PT_VIGNETTE; ++i )
		SetParamGroup( i, "Geometry" );

	FFGLLog::LogToHost( "Created Old Cathode effect" );

	diag::init();
}

FFResult OldCathode::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

float OldCathode::elapsedSeconds() const
{
	if( hostTimeSeen )
		return static_cast< float >( hostTime );

	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration< float >( now - startTime ).count();
}

bool OldCathode::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &resampleShader, shaders::kResampleFragment, "resample" },
		{ &signalShader, shaders::kSignalFragment, "signal" },
		{ &phosphorShader, shaders::kPhosphorFragment, "phosphor" },
		{ &bloomShader, shaders::kBloomFragment, "bloom" },
		{ &blurShader, shaders::kBlurFragment, "blur" },
		{ &tubeShader, shaders::kTubeFragment, "tube" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was.
			diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "Old Cathode: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult OldCathode::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Old Cathode: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	phosphorIndex = 0;
	frameIndex = 0.0f;

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult OldCathode::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const float outputWidth  = std::max( 1.0f, static_cast< float >( hostViewport[ 2 ] ) );
	const float outputHeight = std::max( 1.0f, static_cast< float >( hostViewport[ 3 ] ) );

	const SystemSpec& sys = system( static_cast< int >( params[ PT_SYSTEM ] + 0.5f ) );
	const SourceSpec& src = source( static_cast< int >( params[ PT_SOURCE ] + 0.5f ) );

	const int signalW = sys.signalWidth();
	const int signalH = sys.signalHeight();
	const int bloomW  = std::max( 1, signalW / 4 );
	const int bloomH  = std::max( 1, signalH / 4 );

	//16-bit float rather than 8-bit: the composite waveform swings outside 0..1
	//once chroma is riding on it, and quantising it to 256 levels mid-chain
	//would band the decode.
	if( !resampleBuffer.Ensure( signalW, signalH, GL_RGBA16F )
	    || !signalBuffer.Ensure( signalW, signalH, GL_RGBA16F )
	    || !phosphorBuffer[ 0 ].Ensure( signalW, signalH, GL_RGBA16F )
	    || !phosphorBuffer[ 1 ].Ensure( signalW, signalH, GL_RGBA16F )
	    || !bloomBuffer[ 0 ].Ensure( bloomW, bloomH, GL_RGBA16F )
	    || !bloomBuffer[ 1 ].Ensure( bloomW, bloomH, GL_RGBA16F )
	    || !bloomBuffer[ 2 ].Ensure( bloomW, bloomH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	const float time         = elapsedSeconds();
	const bool usePhosphor   = params[ PT_PERSISTENCE ] > 0.001f;
	const bool useHalation   = params[ PT_HALATION ] > 0.001f;

	//------------------------------------------------------------------
	// 1. Onto the standard's raster, band-limited on the way.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( resampleBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		resampleBuffer.ResizeViewPort();
		ScopedShaderBinding shader( resampleShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		resampleShader.Set( "InputTexture", 0 );
		resampleShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		resampleShader.Set( "InputSize", static_cast< float >( input.Width ), static_cast< float >( input.Height ) );
		resampleShader.Set( "TargetSize", static_cast< float >( signalW ), static_cast< float >( signalH ) );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 2. Encode, damage, decode.
	//------------------------------------------------------------------
	{
		const float lumaMHz = ( src.lumaMHz > 0.0f ? src.lumaMHz : sys.nominalLumaMHz )
		                      * lerp( 0.25f, 1.25f, params[ PT_LUMA_BANDWIDTH ] );
		const float chromaMHz = ( src.chromaMHz > 0.0f ? src.chromaMHz : sys.nominalChromaMHz )
		                        * lerp( 0.25f, 1.25f, params[ PT_CHROMA_BANDWIDTH ] );

		//MHz -> cycles per active line -> cycles per sample, which is what the
		//filter kernels want.
		const float toNormalised = sys.activeLineMicroseconds / static_cast< float >( signalW );

		ScopedFBOBinding fbo( signalBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		signalBuffer.ResizeViewPort();
		ScopedShaderBinding shader( signalShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( resampleBuffer.GetTextureInfo().Handle );

		signalShader.Set( "SourceTexture", 0 );
		signalShader.Set( "MaxUV", 1.0f, 1.0f );
		signalShader.Set( "SignalSize", static_cast< float >( signalW ), static_cast< float >( signalH ) );

		signalShader.Set( "SamplePhaseStep", sys.samplePhaseStep() );
		signalShader.Set( "LinePhaseStep", sys.linePhaseStep() );
		signalShader.Set( "FramePhaseStep", sys.framePhaseStep() );
		signalShader.Set( "PalMode", sys.pal ? 1.0f : 0.0f );

		signalShader.Set( "LumaCutoff", std::clamp( lumaMHz * toNormalised, 0.01f, 0.49f ) );
		signalShader.Set( "ChromaCutoff", std::clamp( chromaMHz * toNormalised, 0.004f, 0.49f ) );
		signalShader.Set( "ChromaDelay", src.chromaDelaySamples );
		signalShader.Set( "NotchAmount", 1.0f - params[ PT_DOT_CRAWL ] );
		signalShader.Set( "Saturation", params[ PT_SATURATION ] * 2.0f );
		//Plus or minus forty degrees, which is about as far as a receiver's hue
		//control would swing before the picture stopped being watchable.
		signalShader.Set( "PhaseError", ( params[ PT_TINT ] - 0.5f ) * 1.4f );

		signalShader.Set( "Noise", params[ PT_NOISE ] * src.noiseScale );
		signalShader.Set( "Dropouts", params[ PT_DROPOUTS ] );
		signalShader.Set( "GhostAmount", params[ PT_GHOSTING ] );
		signalShader.Set( "GhostDelay", params[ PT_GHOST_DELAY ] * 48.0f );
		signalShader.Set( "Interference", params[ PT_INTERFERENCE ] );
		signalShader.Set( "Hum", params[ PT_HUM ] );

		signalShader.Set( "VerticalHold", params[ PT_VERTICAL_HOLD ] );
		signalShader.Set( "Jitter", params[ PT_JITTER ] );
		signalShader.Set( "Tracking", params[ PT_TRACKING ] );
		signalShader.Set( "HeadSwitch", params[ PT_HEAD_SWITCH ] );
		signalShader.Set( "Interlace", params[ PT_INTERLACE ] );

		signalShader.Set( "Time", time );
		signalShader.Set( "FrameIndex", frameIndex );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 3. Phosphor decay. Skipped entirely at zero, which is why the cheap
	//    path stays cheap.
	//------------------------------------------------------------------
	GLuint displayTexture = signalBuffer.GetTextureInfo().Handle;
	if( usePhosphor )
	{
		const int target  = phosphorIndex;
		const int history = 1 - phosphorIndex;

		ScopedFBOBinding fbo( phosphorBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		phosphorBuffer[ target ].ResizeViewPort();
		ScopedShaderBinding shader( phosphorShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, signalBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, phosphorBuffer[ history ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		const float decay = params[ PT_PERSISTENCE ] * 0.93f;
		phosphorShader.Set( "CurrentTexture", 0 );
		phosphorShader.Set( "HistoryTexture", 1 );
		phosphorShader.Set( "MaxUV", 1.0f, 1.0f );
		//Blue goes first, green hangs on longest. A white object dragged across
		//the screen leaves a faintly green wake, which is the detail that makes
		//the trail read as a tube rather than as a feedback buffer.
		phosphorShader.Set( "Decay", decay * 0.97f, decay, decay * 0.90f );
		quad.Draw();

		displayTexture = phosphorBuffer[ target ].GetTextureInfo().Handle;
		phosphorIndex  = history;

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
	}

	//------------------------------------------------------------------
	// 4. Halation: bright pass, then a separable blur, all at quarter size.
	//------------------------------------------------------------------
	if( useHalation )
	{
		{
			ScopedFBOBinding fbo( bloomBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			bloomBuffer[ 0 ].ResizeViewPort();
			ScopedShaderBinding shader( bloomShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( displayTexture );

			bloomShader.Set( "SourceTexture", 0 );
			bloomShader.Set( "MaxUV", 1.0f, 1.0f );
			bloomShader.Set( "SourceSize", static_cast< float >( signalW ), static_cast< float >( signalH ) );
			bloomShader.Set( "Threshold", 0.5f );
			quad.Draw();
		}

		const struct
		{
			int from;
			int to;
			float dx;
			float dy;
		} blurs[] = {
			{ 0, 1, 1.0f / static_cast< float >( bloomW ), 0.0f },
			{ 1, 2, 0.0f, 1.0f / static_cast< float >( bloomH ) },
		};

		for( const auto& pass : blurs )
		{
			ScopedFBOBinding fbo( bloomBuffer[ pass.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			bloomBuffer[ pass.to ].ResizeViewPort();
			ScopedShaderBinding shader( blurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( bloomBuffer[ pass.from ].GetTextureInfo().Handle );

			blurShader.Set( "SourceTexture", 0 );
			blurShader.Set( "MaxUV", 1.0f, 1.0f );
			blurShader.Set( "Direction", pass.dx, pass.dy );
			quad.Draw();
		}
	}

	//------------------------------------------------------------------
	// 5. The tube, straight into whatever the host handed us.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( tubeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, displayTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, bloomBuffer[ 2 ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		tubeShader.Set( "SignalTexture", 0 );
		tubeShader.Set( "BloomTexture", 1 );
		tubeShader.Set( "MaxUV", 1.0f, 1.0f );
		tubeShader.Set( "SignalSize", static_cast< float >( signalW ), static_cast< float >( signalH ) );
		tubeShader.Set( "OutputSize", outputWidth, outputHeight );

		const MaskSpec& maskSpec = mask( static_cast< int >( params[ PT_MASK_PATTERN ] + 0.5f ) );
		tubeShader.Set( "MaskPattern", params[ PT_MASK_PATTERN ] );
		tubeShader.Set( "MaskPitch", lerp( 2.0f, 14.0f, params[ PT_MASK_PITCH ] ) );
		tubeShader.Set( "MaskStrength", params[ PT_MASK_STRENGTH ] );
		tubeShader.Set( "MaskSpill", maskSpec.spill );
		tubeShader.Set( "MaskGain", maskSpec.gain );

		tubeShader.Set( "Scanlines", params[ PT_SCANLINES ] );
		tubeShader.Set( "BeamBloom", params[ PT_BEAM_BLOOM ] );
		tubeShader.Set( "Halation", useHalation ? params[ PT_HALATION ] * 0.8f : 0.0f );
		tubeShader.Set( "Brightness", params[ PT_BRIGHTNESS ] * 2.0f );
		tubeShader.Set( "Contrast", params[ PT_CONTRAST ] * 2.0f );

		tubeShader.Set( "Curvature", params[ PT_CURVATURE ] * 0.6f );
		tubeShader.Set( "CornerRadius", params[ PT_CORNER_RADIUS ] * 0.35f );
		//Plus or minus about fifty degrees. Past that the near edge of the
		//screen is closer to the eye than the focal length and the projection
		//stops meaning anything.
		tubeShader.Set( "PerspectiveX", ( params[ PT_PERSPECTIVE_X ] - 0.5f ) * 1.8f );
		tubeShader.Set( "PerspectiveY", ( params[ PT_PERSPECTIVE_Y ] - 0.5f ) * 1.8f );
		tubeShader.Set( "Zoom", lerp( 0.5f, 1.5f, params[ PT_ZOOM ] ) );
		tubeShader.Set( "Vignette", params[ PT_VIGNETTE ] );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//Wrapped rather than left to grow: the subcarrier phase is accumulated from
	//this, and a float that has run for a few hours of playback has no
	//fractional bits left to carry a quarter-cycle. The wrap is a multiple of
	//both field sequences, so the pattern does not jump when it happens.
	frameIndex += 1.0f;
	if( frameIndex >= 100000.0f )
		frameIndex = 0.0f;

	return FF_SUCCESS;
}

void OldCathode::releaseBuffers()
{
	resampleBuffer.Destroy();
	signalBuffer.Destroy();
	for( auto& buffer : phosphorBuffer )
		buffer.Destroy();
	for( auto& buffer : bloomBuffer )
		buffer.Destroy();
}

FFResult OldCathode::DeInitGL()
{
	resampleShader.FreeGLResources();
	signalShader.FreeGLResources();
	phosphorShader.FreeGLResources();
	bloomShader.FreeGLResources();
	blurShader.FreeGLResources();
	tubeShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	return FF_SUCCESS;
}

FFResult OldCathode::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float OldCathode::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

char* OldCathode::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}
