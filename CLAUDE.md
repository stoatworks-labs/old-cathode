# old-cathode

Analogue television signal path + CRT display as an FFGL effect for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. Public MIT repo.

Read `AGENTS.md` before changing the signal maths.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/octest --out /tmp/frame.png`
- List parameters: `./build/octest --list`

## OpenFX build
- `source/ofx/OldCathodeOFX.cpp` → `build/OldCathode.ofx.bundle` (target
  `OldCathodeOFX`, `-DBUILD_OFX=OFF` to skip) for Resolve/Vegas/Nuke/Natron.
  Standards.cpp links straight from source; the five GPU stages are mirrored
  on the CPU. Change a stage's GLSL, change the matching function there.
- FrameIndex is the timeline frame (OFX time), so the subcarrier's phase walk
  is deterministic against the edit. Persistence is reconstructed from up to
  12 previous frames via OFX temporal clip access.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.oldcathode --size 640x360 --out /tmp/oc.bmp`
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Notes
- Six shader stages; the effect is the GLSL, the C++ is host glue and the
  standards table.
- Signal stages run at the SD raster (754×486 / 921×576) regardless of
  composition size. That is deliberate — see AGENTS.md.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- Mask gains in `source/Standards.cpp` are **measured** with
  `octest --flat 0.05 --measure`. Re-measure after changing a mask shape.
- `flat` and `active` are GLSL reserved words. Shader errors only surface at
  runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It names the stage and logs the GL
vendor/renderer/version next to it.
