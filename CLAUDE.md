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
