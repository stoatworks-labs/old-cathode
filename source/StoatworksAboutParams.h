/*
 * Stoatworks Labs - the About surface for the FFGL plugins.
 *
 * This file is the MASTER, in stoatworks-backend/about/ffgl. It is vendored
 * into each FFGL repo by ../../scripts/sync-about.py - edit it THERE and re-run
 * the sync, never the copies. The facts come from StoatworksAbout.h beside it,
 * which is generated from the website's projects.json.
 *
 * ------------------------------------------------------ why this is not a window
 *
 * An FFGL 2.x plugin has no window and cannot make one. It declares parameters;
 * Resolume draws them, in Resolume's own layout, and that is the entire surface
 * a plugin gets. So there is no About dialog here and there is no mark behind
 * it - those need a UI, and this API has none.
 *
 * What it does have is a text parameter, which the host shows as a labelled
 * value, and an event parameter, which the host shows as a button. That is
 * enough for the same six facts: the name and version go in the text, and the
 * guide, the project page, the source and the funding page each get a button
 * that opens the browser. Anything more ambitious would mean drawing our own UI
 * into the plugin's output texture, which would be putting a dialog on the
 * programme feed.
 *
 * ------------------------------------------------------------------ using it
 *
 * Extend the plugin's ParamID enum with the block, in the constructor declare
 * them, and forward the two callbacks:
 *
 *     enum ParamID : FFUInt32 {
 *         ...
 *         PT_ABOUT_FIRST,
 *         PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
 *     };
 *
 *     // constructor, after the plugin's own SetParamInfof calls. Inline
 *     // rather than a helper: SetParamInfo is protected on CFFGLPlugin, so
 *     // nothing outside the class can call it.
 *     SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, "" );
 *     FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
 *     for( const auto& b : stoatworks::about::buttons() )
 *         SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
 *
 *     // SetFloatParameter
 *     if( index >= PT_ABOUT_FIRST )
 *         return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value )
 *                    ? FF_SUCCESS : FF_FAIL;
 *
 *     // GetTextParameter - the returned buffer must outlive the call, so the
 *     // plugin keeps the string as a member.
 *     if( index == PT_ABOUT_FIRST )
 *     {
 *         aboutText = stoatworks::about::textParam( 0 );
 *         return const_cast< char* >( aboutText.c_str() );
 *     }
 *
 * ------------------------------------------------------------------- ASCII
 *
 * Every string here is ASCII, matching the generated header. MSVC warns on a
 * UTF-8 source without /utf-8, and these build on all three platforms.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "StoatworksAbout.h"

namespace stoatworks::about
{

/*
 * The buttons, in the order the host shows them. A link the product does not
 * have - a guide that is not written, a repo that is still private - is left
 * out of the list rather than shown as a button that opens a 404.
 *
 * All of it is constexpr because a parameter id has to be: the host is told how
 * many parameters a plugin has before any of this could run, and the plugin's
 * own enum has to name the block's size.
 */
struct Button
{
	const char* label;
	const char* url;
};

inline constexpr bool present( const char* s )
{
	return s != nullptr && s[ 0 ] != '\0';
}

/**
	One funding button rather than four.

	The host draws these in the effect's parameter list, beside the controls
	somebody is using mid-show, and four donation buttons in there would be
	shouting. It goes to the website's support page, which lists all four.
*/
inline constexpr const char* kSupportUrl = "https://stoatworks-labs.com/support";

inline constexpr FFUInt32 kButtonCount =
	( present( guide ) ? 1u : 0u ) + ( present( page ) ? 1u : 0u ) + ( present( repo ) ? 1u : 0u ) + 1u;

/// The text line, then one button each. Use this to size the plugin's enum.
inline constexpr FFUInt32 kParamCount = 1u + kButtonCount;

inline const std::array< Button, kButtonCount >& buttons()
{
	static const std::array< Button, kButtonCount > list = [] {
		std::array< Button, kButtonCount > out{};
		std::size_t i = 0;
		if constexpr( present( guide ) )
			out[ i++ ] = { "User guide", guide };
		if constexpr( present( page ) )
			out[ i++ ] = { "Project page", page };
		if constexpr( present( repo ) )
			out[ i++ ] = { "Source on GitHub", repo };
		out[ i++ ] = { "Support the work", kSupportUrl };
		return out;
	}();
	return list;
}

/// Open a URL in the user's browser.
///
/// A plugin shelling out is not pretty, but a plugin has no host API for this
/// and the alternative is a URL the user has to copy off the screen by hand.
/// The URLs are compile-time constants from the generated header, never
/// anything a project file or a host could supply, so there is nothing here for
/// a crafted string to reach.
inline void openUrl( const char* url )
{
#if defined( _WIN32 )
	std::string command = std::string( "start \"\" \"" ) + url + "\"";
#elif defined( __APPLE__ )
	std::string command = std::string( "open \"" ) + url + "\"";
#else
	std::string command = std::string( "xdg-open \"" ) + url + "\" &";
#endif
	std::system( command.c_str() );
}

/// The text line: what this is and what version of it is loaded.
inline std::string textParam( FFUInt32 offset )
{
	if( offset != 0 )
		return {};

	std::string out = std::string( name ) + " " + versionFallback;
	if( std::string( licence ).size() )
		out += " - " + std::string( licence );
	return out;
}

/**
	Handle a press. `offset` is the id relative to the block's first parameter.

	Returns true when the parameter belonged to this block, so the caller can
	return FF_SUCCESS without a second range check.
*/
inline bool handleParam( FFUInt32 offset, float value )
{
	if( offset == 0 )
		return true;// the text line, nothing to do

	const std::size_t index = static_cast< std::size_t >( offset ) - 1;
	if( index >= kButtonCount )
		return false;

	// An event parameter arrives as 1.0 on press and 0.0 on release; opening
	// the browser on both would open it twice.
	if( value >= 0.5f )
		openUrl( buttons()[ index ].url );

	return true;
}

} // namespace stoatworks::about
