"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Two things that will fool you, both of which have already happened here:

  * **Render big enough.** The scanline and mask anti-aliasing fade out on
    fwidth. At 480x360 a 486-line raster is 0.74 output pixels per scan line,
    which is genuinely unresolvable, so Scanlines correctly does nothing and
    looks broken. 1280x960 gives 1.98 and is safely inside the working range.
  * **Watch for controls masked by other controls.** Corner Radius once measured
    dead because the curvature crop had already blacked out the corners it cuts.
    That was a real design flaw rather than a test artefact, and it was fixed by
    overscanning. If something reads dead, work out what is hiding it before
    assuming the test is wrong.
"""
import subprocess, zlib, struct, sys, os, tempfile

SC = tempfile.mkdtemp(prefix="ocsweep")

# A baseline with every stage active, so nothing is dead merely because the
# thing it modifies is switched off.
BASE = {
    "Ghosting": 0.5, "Mask Pattern": 1, "Mask Strength": 0.7, "Scanlines": 0.6,
    "Halation": 0.4, "Persistence": 0.3, "Noise": 0.15, "Dropouts": 0.3,
    "Interference": 0.3, "Vertical Hold": 0.15, "Tracking": 0.3, "Hum": 0.3,
    "Head Switch": 0.3, "Jitter": 0.2, "Curvature": 0.3, "Vignette": 0.3,
    "Corner Radius": 0.2,
}

def render(path, overrides):
    args = ["./build/octest", "--out", path, "--width", "1280", "--height", "960", "--frames", "5"]
    merged = dict(BASE); merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr); sys.exit(1)
    return open(path, "rb").read()

def pixels(png):
    i = 8; idat = b""; w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i+4])[0]; t = png[i+4:i+8]; d = png[i+8:i+8+ln]
        if t == b"IHDR": w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT": idat += d
        i += 12 + ln
    raw = zlib.decompress(idat); stride = w*4
    return b"".join(raw[y*(stride+1)+1:(y+1)*(stride+1)] for y in range(h))

def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa); changed = 0; total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i]-pb[i]), abs(pa[i+1]-pb[i+1]), abs(pa[i+2]-pb[i+2]))
        if d > 2: changed += 1
        total += d
    return changed / (n/4) * 100, total / (n/4)

names = subprocess.run(["./build/octest", "--list"], capture_output=True, text=True).stdout
params = [' '.join(l.split()[1:-1]) for l in names.strip().splitlines()]

# The About block is a text field and browser buttons, declared last. They
# never touch a pixel, so sweeping them only buries a real dead control.
if "About" in params:
    params = params[:params.index("About")]

# Options are discrete; sweep them across their real element range.
DISCRETE = {"System": (0, 1), "Source": (0, 3), "Mask Pattern": (1, 4), "Interlace": (0, 1)}

print(f"{'parameter':<20} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    a = render(f"{SC}/a.png", {p: lo})
    b = render(f"{SC}/b.png", {p: hi})
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok: dead.append(p)
    print(f"{p:<20} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
