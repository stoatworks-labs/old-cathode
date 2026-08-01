/**
    octest -- render the chain offline and write a PNG.

    This machine cannot drive Resolume, and "it compiled" is not evidence that a
    six-stage shader chain does what the comments claim. So the harness builds a
    headless GL 4.1 core context, drives the real OldCathode class through the
    real FFGL entry sequence, and writes out the frame.

    The default test picture is chosen to make wrong answers visible rather than
    to look nice. Colour bars show whether the encode/decode round trip returns
    the hues it was given; a frequency sweep shows where the luma filter gives
    up and, at the top end, produces the cross-colour rainbows that are the
    whole point of modelling the subcarrier; a hard edge shows chroma bleed; a
    ramp shows whether the decoder is linear. A photograph would hide every one
    of those.

        octest --list
        octest --out /tmp/frame.png --width 1280 --height 720
        octest --set "Mask Pattern=2" --set "Tracking=0.6" --frames 30
*/

#include "OldCathode.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The test picture.
//---------------------------------------------------------------------------
/// Writes with y measured from the top, which is how the pattern below is
/// described and how the PNG will be read. GL puts row zero at the bottom of a
/// texture, so the row index is flipped here rather than leaving a flip
/// somewhere in the middle of the chain where it would be a permanent trap.
void setPixel( std::vector< unsigned char >& image, int width, int height, int x, int y, float r, float g, float b )
{
	const size_t i = ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4;
	image[ i + 0 ] = static_cast< unsigned char >( std::lround( std::fmin( std::fmax( r, 0.0f ), 1.0f ) * 255.0f ) );
	image[ i + 1 ] = static_cast< unsigned char >( std::lround( std::fmin( std::fmax( g, 0.0f ), 1.0f ) * 255.0f ) );
	image[ i + 2 ] = static_cast< unsigned char >( std::lround( std::fmin( std::fmax( b, 0.0f ), 1.0f ) * 255.0f ) );
	image[ i + 3 ] = 255;
}

/// A uniform field. Nothing to look at, but it is the only way to measure what
/// the chain does to overall level -- which matters because a shadow mask
/// blocks most of the light that hits it, and the compensation for that is a
/// constant per pattern that has to be measured rather than guessed.
std::vector< unsigned char > buildFlatPicture( int width, int height, float level )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			setPixel( image, width, height, x, y, level, level, level );
	return image;
}

std::vector< unsigned char > buildTestPicture( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	//75% bars: the level a real generator puts out, not 100%, because 100%
	//saturated bars overmodulate composite and would be testing the clipping
	//rather than the decoder.
	const float bars[ 7 ][ 3 ] = {
		{ 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f }, { 0.0f, 0.75f, 0.0f },
		{ 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.75f }
	};

	const int barsEnd  = height * 50 / 100;
	const int sweepEnd = height * 68 / 100;
	const int rampEnd  = height * 82 / 100;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( width );

			if( y < barsEnd )
			{
				const int bar = static_cast< int >( u * 7.0f );
				const float* c = bars[ bar < 0 ? 0 : ( bar > 6 ? 6 : bar ) ];
				setPixel( image, width, height, x, y, c[ 0 ], c[ 1 ], c[ 2 ] );
			}
			else if( y < sweepEnd )
			{
				//A frequency sweep in luminance only. Everything here is grey on
				//the way in; any colour on the way out is cross-colour, which is
				//exactly what should happen where the sweep passes through the
				//subcarrier frequency.
				const float cycles = 4.0f + u * u * 340.0f;
				const float v = 0.5f + 0.45f * std::sin( u * cycles * 6.2831853f );
				setPixel( image, width, height, x, y, v, v, v );
			}
			else if( y < rampEnd )
			{
				//Ramp on the left, hard edges on the right: linearity, then
				//ringing and chroma bleed.
				if( u < 0.6f )
				{
					const float v = u / 0.6f;
					setPixel( image, width, height, x, y, v, v, v );
				}
				else
				{
					const bool on = static_cast< int >( ( u - 0.6f ) * 20.0f ) % 2 == 0;
					setPixel( image, width, height, x, y, on ? 1.0f : 0.0f, on ? 1.0f : 0.0f, on ? 1.0f : 0.0f );
				}
			}
			else
			{
				//A bright caption on black. Halation, beam bloom and the
				//black-level lift next to a highlight all show up here.
				const float cx = ( u - 0.5f ) * 4.0f;
				const float cy = ( static_cast< float >( y - rampEnd ) / static_cast< float >( height - rampEnd ) - 0.5f ) * 4.0f;
				const bool box = std::fabs( cx ) < 0.5f && std::fabs( cy ) < 0.6f;
				const float v = box ? 1.0f : 0.02f;
				setPixel( image, width, height, x, y, v, v, v );
			}
		}
	}

	return image;
}

//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

void usage()
{
	std::printf(
		"octest -- render the Old Cathode chain to a PNG\n"
		"\n"
		"  --out PATH        where to write (default /tmp/oldcathode.png)\n"
		"  --width N         output width (default 1280)\n"
		"  --height N        output height (default 720)\n"
		"  --frames N        frames to run before capturing (default 8)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1\n"
		"  --alpha           keep the alpha channel instead of compositing on black\n"
		"  --flat V          render a uniform field at level V instead of the test card\n"
		"  --measure         print the mean RGB of the middle of the picture\n"
		"  --list            print every parameter and its default, then exit\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/oldcathode.png";
	int width = 1280;
	int height = 720;
	int frames = 8;
	bool keepAlpha = false;
	bool listOnly = false;
	bool measure = false;
	float flatLevel = -1.0f;
	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outputPath = next();
		else if( arg == "--width" )
			width = std::atoi( next().c_str() );
		else if( arg == "--height" )
			height = std::atoi( next().c_str() );
		else if( arg == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( arg == "--alpha" )
			keepAlpha = true;
		else if( arg == "--measure" )
			measure = true;
		else if( arg == "--flat" )
			flatLevel = std::strtof( next().c_str(), nullptr );
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "octest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "octest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "octest: width, height and frames must all be positive\n" );
		return 2;
	}

	OldCathode plugin;

	//The names come from the plugin's own declaration rather than from a table
	//here, so a parameter that is renamed or reordered cannot leave the harness
	//quietly setting the wrong one.
	auto indexOfParameter = [ & ]( const std::string& name ) -> int {
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	};

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-20s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	for( const auto& override : overrides )
	{
		const int index = indexOfParameter( override.first );
		if( index < 0 )
		{
			std::fprintf( stderr, "octest: no parameter named '%s' (try --list)\n", override.first.c_str() );
			return 2;
		}
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), override.second );
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "octest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	std::printf( "GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	//Input picture.
	const std::vector< unsigned char > picture = flatLevel >= 0.0f
	                                                 ? buildFlatPicture( width, height, flatLevel )
	                                                 : buildTestPicture( width, height );
	GLuint inputTexture = 0;
	glGenTextures( 1, &inputTexture );
	glBindTexture( GL_TEXTURE_2D, inputTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//Somewhere to put the result: this is the host's framebuffer as far as the
	//plugin is concerned.
	GLuint outputTexture = 0;
	GLuint outputFBO = 0;
	glGenTextures( 1, &outputTexture );
	glBindTexture( GL_TEXTURE_2D, outputTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &outputFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "octest: output framebuffer is incomplete\n" );
		return 1;
	}

	FFGLViewportStruct viewport = { 0, 0, static_cast< FFUInt32 >( width ), static_cast< FFUInt32 >( height ) };
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "octest: InitGL failed -- see the diagnostics log for which shader\n" );
		return 1;
	}

	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle = inputTexture;
	FFGLTextureStruct* inputs[ 1 ] = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures = 1;
	process.inputTextures = inputs;
	process.HostFBO = outputFBO;

	//Run a few frames rather than one. Persistence reads its own history, the
	//subcarrier's phase walks frame to frame, and the drifting impairments have
	//to have somewhere to have drifted from -- a single frame would be testing
	//none of that. Time comes from the counter, not the clock, so two runs of
	//the harness produce identical pixels.
	for( int frame = 0; frame < frames; ++frame )
	{
		plugin.SetTime( static_cast< double >( frame ) / 60.0 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "octest: ProcessOpenGL failed on frame %d\n", frame );
			return 1;
		}
	}

	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

	const GLenum error = glGetError();
	if( error != GL_NO_ERROR )
		std::fprintf( stderr, "octest: GL error 0x%04x during render\n", error );

	//GL hands back bottom-up; PNG wants top-down.
	std::vector< unsigned char > flipped( pixels.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             pixels.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );

	//The plugin outputs premultiplied alpha, so the colour is already the
	//over-black composite. Flattening is just a matter of forcing alpha opaque.
	if( !keepAlpha )
	{
		for( size_t i = 3; i < flipped.size(); i += 4 )
			flipped[ i ] = 255;
	}

	if( measure )
	{
		//The middle half only. The edges carry the vignette, the curvature crop
		//and the bezel, none of which say anything about what the mask cost.
		double sum[ 3 ] = { 0.0, 0.0, 0.0 };
		size_t counted = 0;
		for( int y = height / 4; y < height * 3 / 4; ++y )
		{
			for( int x = width / 4; x < width * 3 / 4; ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				sum[ 0 ] += flipped[ i + 0 ];
				sum[ 1 ] += flipped[ i + 1 ];
				sum[ 2 ] += flipped[ i + 2 ];
				++counted;
			}
		}
		const double n = static_cast< double >( counted ) * 255.0;
		std::printf( "mean RGB %.4f %.4f %.4f\n", sum[ 0 ] / n, sum[ 1 ] / n, sum[ 2 ] / n );
	}

	if( !writePng( outputPath, width, height, flipped ) )
	{
		std::fprintf( stderr, "octest: could not write %s\n", outputPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outputPath.c_str(), width, height, frames );

	plugin.DeInitGL();
	glDeleteFramebuffers( 1, &outputFBO );
	glDeleteTextures( 1, &outputTexture );
	glDeleteTextures( 1, &inputTexture );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
