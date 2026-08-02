# Old Cathode

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The signal path has been
> verified numerically through an offline render harness that drives the real
> plugin class in a headless GL context — colour bars round-trip to the right
> hues, the mask compensation is measured rather than guessed, and the PAL delay
> line is confirmed to hold hue to 0.0° under a decoder phase error that shifts
> NTSC by up to 26° (see [Status](#status)). It has **not yet been loaded into
> Resolume**, so nothing about how it behaves inside a real host is confirmed.
> Check it in your own rig before trusting it in a show.

An analogue television signal path for [Resolume](https://resolume.com) Arena and
Avenue, as an FFGL effect.

![Broadcast NTSC on a shadow-mask tube](docs/broadcast.jpg)

<sub>Broadcast NTSC, shadow mask. Dot crawl over the bars; the frequency sweep
dies where the luma filter runs out and turns to rainbows where it crosses the
subcarrier; halation around the caption. All rendered by `octest`, the repo's
offline harness.</sub>

| | |
| --- | --- |
| ![VHS EP with tracking error](docs/vhs.jpg) | ![Off-axis view](docs/perspective.jpg) |
| VHS EP: tracking band, dropouts, head-switch tear, colour arriving late | Perspective, curvature and vignette — the mask foreshortens with the glass |

![The four mask patterns](docs/masks.png)

<sub>Shadow mask · aperture grille · slot mask · RGB stripe</sub>

Old Cathode is not a stack of overlays. It encodes the picture onto a colour
subcarrier the way a broadcast encoder would have, damages the resulting
composite waveform as a single signal, and then decodes it again with a
synchronous demodulator that has no more information than a real receiver had.
Only after that does it paint the result onto a phosphor screen with a shadow
mask, a beam that defocuses when it is bright, a curved glass face, and a viewer
sitting somewhere in particular.

Almost everything you recognise as "the CRT look" is a **consequence** of that
path rather than a feature that was drawn on:

| What you see | Why it happens |
| --- | --- |
| **Dot crawl** | The subcarrier sits *inside* the luminance band. Unless the set notches it out, it is still there when the picture is drawn — and it creeps, because its phase advances every line and every frame. |
| **Cross-colour** | Fine luminance detail near the subcarrier frequency gets demodulated as if it were colour. A herringbone jacket turns into rainbows. |
| **Chroma smear** | Colour is carried in a fraction of the bandwidth luminance gets, so it bleeds sideways across edges. |
| **Coloured snow** | Noise is added to the composite, so the decoder un-mixes it along with everything else. It arrives as grey grain *and* coloured speckle, in the right proportion, without being asked for. |

The practical upshot is that the controls interact the way the real thing did.
Turning the chroma bandwidth down smears colour sideways *and* reduces
cross-colour. Losing signal costs you the colour before it costs you the picture,
because chrominance sits at the top of the band and goes under the noise floor
first. Setting the system to PAL changes what the Tint control does, because a
PAL decoder averages each line with the one above it and a phase error cancels.

## Controls

Grouped in the inspector as **Signal**, **Sync**, **Tube** and **Geometry** —
which is also the order the picture travels.

### Signal

| Control | What it is |
| --- | --- |
| **System** | NTSC or PAL. Sets the subcarrier, the raster (754×486 or 921×576 active samples), and how the phase walks line to line and frame to frame. |
| **Source** | Broadcast, VHS SP, VHS LP or VHS EP. Sets bandwidths, the noise floor, and the colour-under delay that makes tape colour arrive slightly *after* the luminance it belongs to. |
| **Luma / Chroma Bandwidth** | Scales the standard's own figures. **0.75 is exactly what the standard specifies** — below that is a worse channel, above it is a better one than ever existed. |
| **Saturation, Tint** | 0.5 is unity and no phase error. Tint is an error in the receiver's colour reference, so in NTSC it rotates every hue and in PAL it costs you saturation instead. |
| **Dot Crawl** | How much subcarrier the luma filter leaves behind. Down means a notch filter: clean picture, softer detail. That was the actual trade. |
| **Ghosting / Ghost Delay** | Multipath — the same transmission arriving a second time off a building, later and weaker. |
| **Noise** | Thermal noise at the tuner front end, added before the decoder. |
| **Dropouts** | A worn tape losing head contact: brief white dashes. |
| **Interference** | A beat from an adjacent channel, drifting slowly across as a herringbone. |

### Sync

| Control | What it is |
| --- | --- |
| **Vertical Hold** | The field no longer starts where the flyback expects. The picture rolls and takes the blanking bar with it. |
| **Jitter** | Line-to-line timebase error. Vertical edges go ragged. |
| **Tracking** | The head is off the recorded track: a band of hash that walks vertically, with the servo's correction skewing the lines either side of it. |
| **Head Switch** | The tear at the point in the field where playback changes heads — normally just below the bottom of a correctly set-up picture, which is why it is normally invisible. |
| **Hum** | Mains ripple on the supply rails. Lifts the black level in a slow drifting bar and bows the left edge with it. |
| **Interlace** | Only half the lines are refreshed each frame. Fine horizontal detail twitters. |

### Tube

| Control | What it is |
| --- | --- |
| **Mask Pattern** | None, **Shadow Mask** (delta triad of round dots — the consumer television mask), **Aperture Grille** (continuous vertical stripes plus the two damper wires that give a Trinitron away), **Slot Mask** (stripes broken into staggered slots), **RGB Stripe** (hard-edged, not a real mask, but legible at pitches where the others turn to mush). |
| **Mask Pitch / Strength** | Dot pitch in output pixels, and how far from flat. The mask lives on the glass, so it foreshortens with the tube under perspective and stays put when the picture moves. |
| **Scanlines / Beam Bloom** | The beam is a spot with a Gaussian profile, and it gets *fatter where it is brighter* because more current defocuses it. That is why highlights swell into the gaps and shadows do not. |
| **Persistence** | Emissive decay. Blue goes first and green hangs on longest, so a white object dragged across leaves a faintly green wake. |
| **Halation** | Light scattered sideways inside the glass faceplate. |
| **Brightness / Contrast** | 0.5 is unity on both. |

### Geometry

| Control | What it is |
| --- | --- |
| **Curvature** | The face is not flat, so the picture painted on it bulges. Corners crop, as they do on a real tube. |
| **Corner Radius** | The rounded rectangle of the tube face. |
| **Perspective X / Y, Zoom** | A real projection: the eye ray is intersected with the plane of the screen. 0.5 is straight on. |
| **Vignette** | Falloff towards the edge of the glass. |

Outside the tube face the effect outputs **transparent**, not black, so you can
composite a television over whatever is on the layer below.

## Install

Drop the plugin into Resolume's plugin folder and restart it:

- **macOS** — `~/Documents/Resolume Arena/Extra Effects/` (or `Resolume Avenue`)
- **Windows** — `%USERPROFILE%\Documents\Resolume Arena\Extra Effects\`

It appears in the effects list as **Old Cathode**.

The builds are unsigned; see [docs/UNSIGNED.md](docs/UNSIGNED.md) for getting them
past Gatekeeper and SmartScreen.

## Build

```bash
git clone --recurse-submodules https://github.com/stoatworks-labs/old-cathode.git
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

macOS produces a universal (arm64 + x86_64) `OldCathode.bundle`; Windows a
`.dll`. `cmake --install build` drops the bundle straight into the Resolume
folder above.

## Looking at it without Resolume

`octest` renders the real chain to a PNG through a headless GL context. It drives
the actual plugin class through the actual FFGL entry sequence, so it is testing
the shipped code rather than a copy of it.

```bash
./build/octest --out /tmp/frame.png --width 1920 --height 1080
```

```bash
./build/octest --list
```

```bash
./build/octest --out /tmp/vhs.png --set "Source=3" --set "Tracking=0.55" --set "Dropouts=0.5" --frames 40
```

Its default test card is built to make wrong answers visible rather than to look
nice: colour bars to check the round trip, a frequency sweep to find where the
luma filter gives up and where cross-colour starts, a hard edge for chroma bleed,
a ramp for linearity. `--flat` and `--measure` are what the mask gains were
calibrated with.

## Status

Verified through the offline harness on an M4 Max:

- **Level is preserved end to end.** A flat 0.5 field comes back 0.502 through the
  full encode/decode round trip.
- **Colour bars round-trip to the right hues**, with dot crawl visible over them
  and cross-colour appearing exactly where the frequency sweep crosses the
  subcarrier.
- **Mask compensation is measured, not guessed.** Every pattern lands within ~1%
  of the maskless baseline.
- **The PAL delay line works.** Under a 16.8° decoder reference error, NTSC hue
  shifts by 6.5–26° across the bars while PAL holds hue at 0.0° and loses ~8%
  saturation instead — which is the trade PAL exists to make.
- **All 33 controls demonstrably do something.** `tools/sweep.py` renders every
  parameter at both ends of its range and fails if any made no difference — the
  only way to catch a uniform name that does not match between the C++ and the
  GLSL, since that fails silently rather than at build time.
- **Cost is 0.34 ms/frame at 1080p and 0.83 ms at 4K.** The expensive filtering
  runs at the SD raster, so it barely scales with composition size. Both figures
  are from one machine, not from CI.

Not verified:

- **Not yet loaded into Resolume.** Parameter groups, option dropdowns and the
  host's real texture sizes and premultiplication behaviour are all unconfirmed.
- **Windows and the universal macOS build have never been run**, only built.

## Licence

MIT — see [LICENSE](LICENSE).

Built on the [Resolume FFGL SDK](https://github.com/resolume/ffgl).
