#pragma once

namespace oldcathode
{
/**
    The numbers that define a television system.

    These are not taste. They are the figures the broadcast standards actually
    specify, and everything the signal stage does is derived from them rather
    than tuned by hand -- the sample rate, the rate the subcarrier phase
    advances down the picture and between frames, the bandwidth the colour gets
    compared to the luminance. Change the system and the artefacts change with
    it, because the artefacts were always consequences of these numbers.
*/
struct SystemSpec
{
	const char* name;
	float subcarrierMHz;
	float subcarrierCyclesPerLine;//over the *whole* line, blanking included
	float linesPerFrame;
	int activeLines;
	float activeLineMicroseconds;
	float nominalLumaMHz;
	float nominalChromaMHz;
	bool pal;//alternate V line to line and run the delay line on decode

	/// Composite samples per active line, at four times subcarrier.
	///
	/// Four times subcarrier is the rate composite digital video was actually
	/// sampled at, and it is the right choice here for a reason beyond
	/// authenticity: at exactly 4fsc, two samples apart is exactly half a
	/// subcarrier cycle, which makes the chroma notch in the decoder a plain
	/// three-tap average instead of a designed filter.
	int signalWidth() const;
	int signalHeight() const
	{
		return activeLines;
	}

	/// Radians of subcarrier between adjacent samples. Very close to pi/2.
	float samplePhaseStep() const;
	/// Radians of subcarrier between adjacent lines. This is what makes dot
	/// crawl a diagonal pattern rather than a static chequer.
	float linePhaseStep() const;
	/// ...and between frames, which is what makes it crawl.
	float framePhaseStep() const;
};

/**
    What the picture was played back from.

    A broadcast off the aerial and a third-generation tape are the same signal
    standard carrying wildly different bandwidths. VHS in particular records
    chrominance on its own low carrier underneath the luminance, which is why
    its colour is both far softer than broadcast and arrives slightly late --
    the two are worth modelling separately because that lag is a large part of
    why tape looks like tape.
*/
struct SourceSpec
{
	const char* name;
	float lumaMHz;  //0 = whatever the system specifies
	float chromaMHz;//0 = likewise
	float chromaDelaySamples;
	float noiseScale;
};

/**
    A phosphor mask.

    `spill` is the floor: the electron spot is wider than one phosphor, so the
    neighbouring dots of the other two colours are always somewhat lit. Without
    it a mask is three separated primaries, which is both wrong -- no real tube
    resolves that cleanly -- and unusable, because a mask that is opaque between
    dots throws away two thirds of the light and everything above a third grey
    clips trying to get it back.

    `gain` is what makes up the rest, and it is **measured, not derived**: run
    `octest --flat 0.05 --measure` per pattern with the mask at full strength
    and scale until the mean matches the maskless baseline. Deriving it from the
    duty cycle on paper gets the wrong answer, because the shaped edges and the
    anti-aliasing both eat into it. See AGENTS.md.
*/
struct MaskSpec
{
	const char* name;
	float spill;
	float gain;
};

int systemCount();
int sourceCount();
int maskCount();
const SystemSpec& system( int index );
const SourceSpec& source( int index );
const MaskSpec& mask( int index );

} // namespace oldcathode
