# AGENTS.md — bringing an LLM up to speed on Old Cathode

Orientation for an AI assistant (or a new human) picking this project up cold.
`CLAUDE.md` holds the short command reference; this file explains the model and
the traps.

---

## 1. What this is

An **analogue television signal path** for Resolume Arena / Avenue, built on the
official Resolume **FFGL** SDK. C++/GLSL, CMake, public MIT.

The one idea to internalise before changing anything:

> **This models a route, not a look.** Dot crawl, cross-colour, chroma smear and
> coloured snow are not drawn anywhere. They are what happens when you encode a
> picture onto a subcarrier, damage the composite, and decode it again with a
> demodulator that cannot tell luminance from chrominance.

If you are ever tempted to "add" one of those artefacts directly, stop. Either it
already falls out of the chain, or the chain is wrong somewhere and that is the
bug to fix. The whole value of the thing is that the artefacts are correlated
with each other the way the real ones are — dropping the chroma bandwidth softens
colour *and* suppresses cross-colour, because in the hardware those were the same
filter.

## 2. The shape of it

Six shader stages. `source/Shaders.h` documents them; each lives in its own file
under `source/shaders/`.

```
Resample   full-res RGB -> the standard's SD raster, band-limited first
Signal     encode to composite, damage it, decode it back
Phosphor   emissive decay (skipped when Persistence is 0)
Bloom      halation: bright pass + separable blur at quarter size (skipped at 0)
Tube       mask, scanning beam, curvature, perspective, vignette
```

`source/Standards.cpp` holds the numbers the whole thing is derived from — the
subcarrier frequencies, cycles per line, active line durations, bandwidths. The
sample rate, the phase step per sample, the phase walk per line and per frame are
all *computed* from those. **Do not hand-tune anything that is currently
derived**; change the standard's figure instead and let it propagate.

### Why the signal stages run at SD

The signal buffers are 754×486 (NTSC) or 921×576 (PAL) regardless of the
composition size. Two reasons, and the second is the load-bearing one:

1. It is authentic. That *is* the resolution.
2. **It makes the artefacts resolution-independent.** Dot crawl driven by the
   composition size would crawl differently every time someone changed their
   output resolution. It also makes the 17-tap filtering affordable: 4K costs
   only 2.4× what 1080p does, because the expensive stage does not scale.

754 is four times subcarrier, and that is not decoration. **At exactly 4fsc, two
samples apart is exactly half a subcarrier cycle**, which turns the decoder's
chroma notch into a plain three-tap average instead of a designed filter. If you
change the sample rate away from 4fsc, the notch stops working and dot crawl
becomes uncontrollable.

## 3. Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Add `-DCMAKE_OSX_ARCHITECTURES=arm64` for a much faster dev build.

## 4. Traps

### The macOS one that will get you

**`CMAKE_OSX_ARCHITECTURES` must be set before the first target is created.** Set
it later and CMake silently ignores it — you get an arm64-only binary that the
build log calls a success, and an Intel Resolume that quietly fails to load the
plugin.

**Always verify the artefact, never the log:**

```bash
lipo -archs build/OldCathode.bundle/Contents/MacOS/OldCathode
```

### GLSL reserved words

`flat` and `active` are reserved. So are `filter`, `input`, `output`, `sample`,
`common`, `partition`, `resource` and a long tail of others. Two of these were
already hit here, and the failure mode is nasty: the shader fails to compile at
*runtime*, `InitGL` returns `FF_FAIL`, and Resolume shows an effect that silently
does nothing. That is what `source/Diag.cpp` exists for — it names the stage.

### The SDK leaks its colour texture

`ffglex::FFGLFBO::Release()` deletes the framebuffer and the depth renderbuffer,
then tests `depthBufferID` a second time where it plainly meant `colorTextureID`
(SDK `b1afaf9`, `FFGLFBO.cpp`). The colour texture is never freed.

`source/PassBuffer.{h,cpp}` subclasses around it. **Use `PassBuffer`, never
`FFGLFBO` directly** — this plugin rebuilds every buffer whenever the system or
the composition size changes, so the leak would be per-comparison, not one-off.

### `FFGLScopedFBOBinding.h` is not in the umbrella header

`FFGLSDK.h` includes every other scoped binding and omits that one. Include it by
hand.

### The plugin registers itself from a static constructor

`CFFGLPluginInfo` is a file-scope object in `OldCathode.cpp` that nothing
references by name. That is why `oldcathode_core` is an **OBJECT** library and not
a **STATIC** one: in an archive the linker is entitled to drop the whole
translation unit, and you get a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. Do not "tidy" it to STATIC.

### Mask gains are measured, not derived

`kMasks` in `source/Standards.cpp` carries a `gain` per pattern that restores the
light the mask blocks. **Deriving it from the duty cycle on paper gets the wrong
answer** — the shaped edges and the anti-aliasing both eat into it. Re-measure
after any change to a mask's shape:

```bash
./build/octest --flat 0.05 --measure --set "Mask Pattern=1" --set "Mask Strength=1" --set "Scanlines=0" --set "Halation=0" --set "Persistence=0" --set "Noise=0" --set "Jitter=0" --set "Curvature=0" --set "Vignette=0" --set "Corner Radius=0"
```

Compare against the same command with `Mask Pattern=0` and scale the gain by the
ratio. Use 0.05 and not a mid grey: at mid grey the peaks clip and the
measurement is meaningless.

The `spill` beside it is the floor — the beam spot is wider than one phosphor, so
its neighbours are always partly lit. It is not cosmetic. Without it a mask needs
a gain of 6.6 to break even, every highlight clips, and a mid grey comes back at
a third of its level.

### Anti-aliasing thresholds are load-bearing

Both the mask and the scanlines fade out on `fwidth`, and the thresholds were
wrong on the first pass in a way that was easy to miss: **486 lines into a
1080-line composition is 2.2 pixels per line**, which is the single most common
case there is, and the original threshold left scanlines at 24% strength there.
If you touch `scanAA` or `maskAA`, check 720p, 1080p and 4K, in both NTSC and
PAL, before believing it.

## 5. Testing

There is no unit test rig and there cannot usefully be one — the output is a
picture. `tools/octest` is the substitute: a headless GL 4.1 core context driving
the **real** `OldCathode` class through the **real** FFGL entry sequence, writing
a PNG. It is faster to iterate on than the host and it can be measured, so reach
for it first even when Resolume is available.

It is deterministic (time comes from the frame counter, not the clock), so two
runs produce identical pixels and a change that was not supposed to alter the
picture can be checked byte for byte.

What the default test card is for, band by band:

- **Colour bars** — does the encode/decode round trip return the hues it was
  given? Dot crawl is visible over them.
- **Frequency sweep** — where does the luma filter give up, and where does
  cross-colour start? It is grey going in, so *any* colour coming out is
  cross-colour.
- **Ramp + hard edges** — linearity, ringing, chroma bleed.
- **Caption on black** — halation, beam bloom, black-level lift.

A photograph would hide every one of those. When changing the signal maths, test
against this, not against footage.

### A dead control is invisible to the compiler

`tools/sweep.py` renders every parameter at both ends of its range and fails if
any of them made no difference to the picture.

```bash
python3 tools/sweep.py
```

**Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL.** A uniform name that does not match is silently ignored —
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op —
so a control can be completely dead while everything compiles, links, loads and
renders. Nothing else here catches that.

It found two suspects on its first run; both were instructive rather than typos.
Scanlines read dead because the sweep was rendering at 480×360, where 486 lines
is 0.74 output pixels each and the anti-aliasing correctly gives up. Corner
Radius read dead because the curvature crop had already blacked out the corners
it cuts — which was a real design flaw, and the reason the tube pass now
overscans.

### Checks worth re-running after any change to the decoder

```bash
# Level must survive the round trip. Expect ~0.502 in and out.
./build/octest --flat 0.5 --measure --set "Mask Pattern=0" --set "Scanlines=0" --set "Halation=0" --set "Persistence=0" --set "Noise=0" --set "Jitter=0" --set "Curvature=0" --set "Vignette=0" --set "Corner Radius=0"
```

For the PAL delay line: render colour bars at `Tint=0.71` under both systems and
measure the hue of each bar. **NTSC should shift hue by 6–26° and hold
saturation; PAL should hold hue at 0.0° and lose ~8% saturation.** If PAL starts
shifting hue, the delay line has broken — most likely the V sign correction and
the averaging have got out of step in `decodeLine`.

## 6. What has never been checked

- **It has not been loaded into Resolume.** Arena *is* installed on the
  development machine and `cmake --install` puts the bundle where it will be
  found, but nothing has been driven through the host. Parameter groups, the
  option dropdowns, and the host's real texture sizes and premultiplication
  behaviour are all unconfirmed — and those are exactly the things the offline
  harness cannot tell you about, because it supplies its own textures.
- **The Windows build and the universal macOS build have never been run**, only
  compiled.
- All performance figures come from one M4 Max, never from CI — hosted macOS
  runners have no GPU.

## 7. Conventions

- Public repo. "Commit" means commit **and** push.
- Standard AI disclaimer in the README — see the fleet's disclaimer scope.

## Diagnostics

`source/Diag.{h,cpp}` is a small member of the fleet's `diag` family: a log file
only. No crash handler (a plugin has no business installing a process-wide signal
handler inside Resolume) and no bundle command (there is no UI to hang one off).

What it covers is the failure that actually happens — `InitGL` returning
`FF_FAIL` because a shader would not compile, which from the operator's side
looks like "the effect does nothing" with no message anywhere. With six stages,
knowing *which* one is most of the diagnosis; the GL vendor/renderer/version
strings sit next to it because that is usually the rest of it.
