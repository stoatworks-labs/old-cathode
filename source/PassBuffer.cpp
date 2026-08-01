#include "PassBuffer.h"

using namespace ffglex;

namespace oldcathode
{
PassBuffer::~PassBuffer()
{
	//Nothing GL can be done here -- a destructor may well run without a current
	//context. DeInitGL is where the release happens; this is only a reminder
	//that it has to.
}

bool PassBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	if( fboID != 0 && width == requestedWidth && height == requestedHeight && internalColorFormat == format )
		return true;

	Destroy();

	if( !Initialise( requestedWidth, requestedHeight, format ) )
		return false;

	//A freshly allocated framebuffer's contents are undefined, and the phosphor
	//pass reads its own previous output. Undefined there is not a subtle
	//artefact: it is whatever texture memory the driver handed back.
	GLint previousFBO = 0;
	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFBO );
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	glBindFramebuffer( GL_FRAMEBUFFER, fboID );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	glBindFramebuffer( GL_FRAMEBUFFER, previousFBO );
	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );

	return true;
}

void PassBuffer::Destroy()
{
	//The SDK's Release() leaves this behind. Delete it first, then let the base
	//class deal with the framebuffer and the depth buffer.
	if( colorTextureID != 0 )
	{
		glDeleteTextures( 1, &colorTextureID );
		colorTextureID = 0;
	}

	Release();
}

} // namespace oldcathode
