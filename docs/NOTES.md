# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*old-cathode — analogue TV signal path + CRT display as an FFGL effect for Resolume; PUBLIC MIT, verified via its own headless octest harness and since 2026-08-02 run in Resolume on real content*

**old-cathode** (started 2026-08-01) — an **analogue television signal path**, not
a CRT look, as an FFGL effect for Resolume Arena/Avenue.
`~/Projects/old-cathode`, **PUBLIC MIT**, github.com/stoatworks-labs/old-cathode,
v0.1.0, no release cut yet.

**The design idea that governs everything:** it encodes to a colour subcarrier,
damages the composite as one signal, and decodes with a synchronous demodulator.
Dot crawl, cross-colour, chroma smear and coloured snow are **consequences, never
drawn**. If tempted to add one directly, the chain is wrong somewhere instead.

**Signal stages run at the SD raster (754×486 NTSC / 921×576 PAL) regardless of
composition size.** Authentic *and* load-bearing: artefacts stay
resolution-independent, and 4K costs only 2.4× 1080p (0.83 vs 0.34 ms/frame, M4
Max only, never CI). 754 = **4× subcarrier**, which is what makes the decoder's
chroma notch a plain three-tap average — two samples apart is exactly 180° of
carrier. Move off 4fsc and the notch stops working.

**Verified only by `tools/octest`** — a headless CGL 4.1-core harness that drives
the real plugin class through the real FFGL sequence to a PNG. Deterministic
(time from the frame counter). Level survives the round trip (0.502 in/out); mask
gains land within ~1%; **PAL delay line holds hue at 0.0° under a 16.8° decoder
phase error that shifts NTSC 6–26°**. **Run in Resolume on real content** as of
2026-08-02 (Allan's own report), so the old "never loaded into Resolume" is
retired along with the param-group/host-texture/premultiplication caveat.

`tools/sweep.py` renders every param at both ends and fails if any made no
difference. **A GLSL uniform name that doesn't match the C++ is silently ignored**
— `glGetUniformLocation` returns -1, `glUniform(-1)` is a documented no-op — so a
control can be stone dead while everything compiles, links and renders. Nothing
else catches it; worth copying to any shader repo in the fleet. All 33 pass.

Traps live in the repo's AGENTS.md — see [agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md). The
sharp ones: **`flat` and `active` are GLSL reserved words** (fails at runtime as
"effect does nothing"); mask gains are **measured with `octest --flat 0.05
--measure`, never derived** (edge shaping and AA eat the duty cycle); the
scanline/mask `fwidth` fade thresholds must keep **486 lines into 1080 = 2.2
px/line** on the full-strength side; and the tube pass **overscans** (curvature
divided through by its corner expansion) because otherwise barrel distortion
pulls the picture's own corners inside the glass and Corner Radius does nothing.

**Resolume Arena IS installed on this Mac** (`/Applications/Resolume Arena`,
plugins in `~/Documents/Resolume Arena/Extra Effects`, where `cmake --install`
puts it and where LumaKey already lives). So "can't test it here" is false — and
as of 2026-08-02 it has been driven through the host. What stays true is that
driving that GUI *from a session* is unreliable per
**screenshot capture** (working-practice note, kept in Claude memory), so host verification comes from Allan running
it, not from an agent.

Related: [resolume luma keyer](https://github.com/stoatworks-labs/resolume-luma-keyer/blob/main/docs/NOTES.md) (`resolume-luma-keyer`) (the FFGL submodule + CMake MODULE
pattern came from there), [resolume ofx bridge](https://github.com/stoatworks-labs/resolume-ofx-bridge/blob/main/docs/NOTES.md) (`resolume-ofx-bridge`),
[ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md), **disclaimer scope** (working-practice note, kept in Claude memory) (AI disclaimer
applies).
