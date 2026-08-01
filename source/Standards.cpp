#include "Standards.h"

#include <algorithm>
#include <cmath>

namespace oldcathode
{
namespace
{
constexpr float kTau = 6.28318530718f;

const SystemSpec kSystems[] = {
	//NTSC-M. 3.579545MHz is 455/2 times the line rate, chosen so the subcarrier
	//sits in the gaps of the luminance spectrum -- which is also exactly why it
	//half-cancels frame to frame and why dot crawl creeps rather than sits.
	{ "NTSC", 3.579545f, 227.5f, 525.0f, 486, 52.6557f, 4.2f, 1.3f, false },
	//PAL-I, the 5.5MHz variant used in the UK and Ireland. B/G is 5.0MHz; the
	//difference is small and the sharper one is the more useful default.
	{ "PAL", 4.43361875f, 283.7516f, 625.0f, 576, 51.9479f, 5.5f, 1.3f, true },
};

const SourceSpec kSources[] = {
	//Straight off the aerial: full standard bandwidth, no colour-under.
	{ "Broadcast", 0.0f, 0.0f, 0.0f, 1.0f },
	//VHS at standard play. Luminance is FM-recorded at about 3MHz; chrominance
	//is heterodyned down to 629kHz, comes back softer than anything in
	//broadcast, and comes back late.
	{ "VHS SP", 3.0f, 0.4f, 1.6f, 1.8f },
	{ "VHS LP", 2.4f, 0.4f, 2.2f, 2.6f },
	{ "VHS EP", 2.0f, 0.35f, 2.8f, 3.4f },
};

//Spill is how much of the beam lands on the neighbouring phosphors. A delta
//mask gets the most -- the spot is round and straddles several triads in both
//axes. A grille gets less, because its stripes are continuous vertically and
//there is nothing to straddle. The RGB stripe is not a real mask and is left
//deliberately harder so it stays legible as a graphic.
//
//The gains beside them are measured, not calculated. Do not tidy them into
//round numbers.
const MaskSpec kMasks[] = {
	{ "None", 0.0f, 1.0f },
	{ "Shadow Mask", 0.35f, 2.256f },
	{ "Aperture Grille", 0.28f, 2.148f },
	{ "Slot Mask", 0.32f, 2.079f },
	{ "RGB Stripe", 0.15f, 2.308f },
};
} // namespace

int SystemSpec::signalWidth() const
{
	return static_cast< int >( std::lround( 4.0f * subcarrierMHz * activeLineMicroseconds ) );
}

float SystemSpec::samplePhaseStep() const
{
	const float cyclesPerActiveLine = subcarrierMHz * activeLineMicroseconds;
	return kTau * cyclesPerActiveLine / static_cast< float >( signalWidth() );
}

float SystemSpec::linePhaseStep() const
{
	//Only the fractional part matters: a whole cycle of subcarrier between lines
	//would be no shift at all.
	float whole = 0.0f;
	return kTau * std::modf( subcarrierCyclesPerLine, &whole );
}

float SystemSpec::framePhaseStep() const
{
	//NTSC lands on half a cycle per frame, giving the four-field sequence; PAL
	//lands on three quarters, giving eight. Both fall out of the arithmetic
	//rather than being special-cased.
	return kTau * std::fmod( subcarrierCyclesPerLine * linesPerFrame, 1.0f );
}

int systemCount()
{
	return static_cast< int >( sizeof( kSystems ) / sizeof( kSystems[ 0 ] ) );
}

int sourceCount()
{
	return static_cast< int >( sizeof( kSources ) / sizeof( kSources[ 0 ] ) );
}

const SystemSpec& system( int index )
{
	return kSystems[ std::clamp( index, 0, systemCount() - 1 ) ];
}

const SourceSpec& source( int index )
{
	return kSources[ std::clamp( index, 0, sourceCount() - 1 ) ];
}

int maskCount()
{
	return static_cast< int >( sizeof( kMasks ) / sizeof( kMasks[ 0 ] ) );
}

const MaskSpec& mask( int index )
{
	return kMasks[ std::clamp( index, 0, maskCount() - 1 ) ];
}

} // namespace oldcathode
