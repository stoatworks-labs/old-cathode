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

`--pipe` reads raw RGBA frames from stdin and writes them to stdout, so real
footage goes through the chain with `ffmpeg | octest | ffmpeg`, and `--script`
automates the parameters over the sequence. That is what the project video is
built from — see `stoatworks-backend/video/projects/old-cathode/render.py`, which
also records why this project renders its footage rather than filming the plugin
in Resolume like every other video in the series films its app.

Two things about `--pipe` that are easy to get wrong. **stdout is the video**, so
anything conversational has to go to stderr or it lands inside a frame. And the
flips do **not** cancel: ffmpeg hands over top-down rows and GL wants bottom-up,
so the frame is flipped on the way in and again on the way out. Skipping both
looks almost right and puts the head-switch tear at the top of the picture
instead of the bottom.

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

- **It has never been rendered through Resolume onto footage.** It *loads* there
  — see below — but loading cleanly is not the same as looking right. Parameter
  groups, the option dropdowns, and Arena's real texture sizes and
  premultiplication behaviour are all unconfirmed, and those are exactly what the
  offline harness cannot tell you about, because it supplies its own textures.
- **The Windows build and the universal macOS build have never been run**, only
  compiled.
- All performance figures come from one M4 Max, never from CI — hosted macOS
  runners have no GPU.

## 6b. Driving Resolume, when you need to

Arena is installed on the development machine and `cmake --install` puts the
bundle where it looks. Two things make it drivable without clicking at
coordinates, and one trap will waste your time.

**Arena is accessible.** Its dialogs answer to
`System Events` by button name — `click button "Remind Me Later" of window 1` —
so the update prompt and the composition confirmations can be dismissed
properly. The custom-drawn buttons inside a confirmation are *not* in the tree;
get the window's `position`/`size` and click a computed point instead, and check
which button you are aiming at first. "New Composition!" offers
**New / Cancel / Save & New** with **Save & New as the highlighted default**, so
never dismiss one with Return.

**Arena has a REST API on `http://127.0.0.1:8080/api/v1`.** `GET .../effects`
is the honest answer to "did the plugin register", and it is how the idstring,
name, category and description in this repo were checked against what the host
actually parsed. `GET .../composition/layers/{n}/clips/{n}` gives the applied
effects and their parameters.

**The trap:** `POST .../clips/{n}/open` **returns 200 and ignores the path you
sent it.** Every body format tried — JSON string, `text/plain`, no content type —
answered 200 and loaded an unrelated file from the operator's own media folder.
It is a silent no-op in the same family as Konnect's `add_via`. Do not build
anything on it, and verify what actually loaded by reading `fileinfo.path` back.
There is also **no endpoint for adding an effect** — `POST .../effects` is 404 —
so applying the effect is a UI job.

**Whatever you do, do not film or modify the operator's own composition.** Check
a composition's file checksum before and after if you touch Arena at all.

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


## The browser demo

`demo/` is a static page at **old-cathode-demo.stoatworks-labs.com**: this
plugin's own GLSL, ported to WebGL2, running on clips generated in the page with
the parameters the constructor declares. It is deployed as a Cloudflare Worker
serving `demo/` as static assets (`wrangler.toml`), with **no build step** — what
is committed is what is served.

Three things about it are not visible from the files:

- **`demo/plugin.js` carries a second copy of the shader.** The demo cannot
  include a C++ file, so the GLSL from `source/shaders/` is duplicated there and
  *nothing enforces that they agree*. Change the shader and change both, or the
  page quietly goes on rendering the old maths.
- **`demo/vendor/` is vendored, not authored here.** The master is
  `stoatworks-backend/resolume-demo/`; fix it there and re-run its `sync.sh`.
  `sync.sh --check` reports drift. A fix applied to the copy fixes one plugin out
  of six.
- **Verify a deploy by content, never by status code.** A wrong page still
  answers 200.

```bash
cf-run npx wrangler deploy
curl -s 'https://old-cathode-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

The page is emphatic that it is not the plugin, and lists what it does not
reproduce in a disclosure at the foot. Keep that: it is a port, so nothing on it
is evidence about the plugin, and the offline harness in this repository is
still the only thing that measures anything.

## Factory presets

### The host owns the parameters, and a preset had to learn that

Reported against **vertigo** as its issue #2 and fixed across all seven plugins
on 2026-08-22: choosing a factory preset in Resolume did nothing and the
dropdown snapped straight back to `Custom`.

The pattern was copy-based — `applyPreset` writes the values into `params[]` and
raises `FF_EVENT_FLAG_VALUE` so the host re-reads its sliders — and it rests on
an assumption FFGL never makes. **The host owns parameter state.** It pushes its
own values back down whenever it likes, and nothing obliges it to act on a value
event. Resolume does not: it carries on restating the values it still believes
in, which are the ones from before the preset. Those restatements arrive as
`SetFloatParameter` calls carrying a changed value, so the rule "a covered
parameter changed, therefore the operator has taken over" fired on the host's
own echo, instantly, every time.

Three things now arrive through that one call while a preset is active, and only
the third is a person:

| What arrives | How it is recognised | What happens |
|---|---|---|
| the preset's own values, from a host that honoured the events | matches the preset | ignored — nothing to write |
| the values from *before* the preset, from a host that did not | matches `hostValues[]`, the host's own last word | ignored — writing it would undo the preset |
| a new value from neither | matches neither | written, and the preset falls back to Custom |

`hostValues[]` is the record of what the **host** last sent, which is not what
the plugin is rendering with, and `seedHostValues()` fills it from the defaults
on the first parameter traffic — **before `applyPreset` can run**. Seeding it
afterwards records the preset's own values as the host's opening position, so
the host's very next restatement looks like an edit; that mistake was made once
during the fix and the test caught it.

Two tolerances matter and they are not the same number. `kSame` is **1e-3**, a
host-quantisation allowance rather than a float epsilon — a host that keeps its
parameters shorter than a float hands back a number *near* ours. The pre-existing
"did a covered parameter move?" test below still works to 1e-4, which is why a
value matching the preset is **ignored rather than written**: letting a rounded
copy of our own value into `params[]` would trip that tighter test.

`octest --presets` drives all three hosts across every preset, with no GL
involved, and runs in `tools/verify.sh`. Against the pre-fix code it fails in
exactly the "ignores value events" column.

`source/Presets.h` is one table of named looks in the host-facing 0..1
parameter space, and it drives **both** builds — the FFGL constructor and the
OFX describe each read it, so a preset cannot drift between Resolume and
Resolve. Element 0 of the dropdown is always **Custom**, which is not in the
table: it means "the sliders are the truth".

The mechanics are deliberately copy-based. Picking a preset copies the table
row into the real parameters — the FFGL side raises `FF_EVENT_FLAG_VALUE` per
changed parameter so the host re-reads its sliders, the OFX side setValues
inside one edit block so undo takes the whole preset back at once. A host that
ignores the events still renders the preset correctly and merely shows stale
knobs. Editing any covered parameter afterwards flips the dropdown back to
Custom — judged by comparing values, not by the change reason, so a host
echoing our own writes cannot un-set the preset.

A preset covers signal, sync, tube and screen shape. Perspective and Zoom are
framing — where the viewer sits is the operator's business. Interlace is a
boolean and preset values for it must be 0 or 1.

Verified by rendering a preset and its hand-set equivalent through the offline
harness and `ofxprobe --edit` (which delivers the set as a real user edit,
with `kOfxActionInstanceChanged`) and comparing byte-for-byte.
