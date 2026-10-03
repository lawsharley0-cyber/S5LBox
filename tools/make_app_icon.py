"""Draw NEON's app icon: an N on the iOS 6-era water-droplet glass.

The icon is generated rather than drawn by hand, so it can be regenerated at
any size and tweaked by editing numbers. Everything is plain SVG built here:
the letter is geometry, not a font, so no font file is needed and the output
does not depend on what the build machine has installed. Headless Chromium
rasterises the SVG and Pillow strips the alpha channel, because an app icon
must be opaque (iOS draws the rounded corners itself, so the square is full
bleed).

Usage:
  python tools/make_app_icon.py [--out app/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png]
                                [--preview preview.png] [--svg icon.svg]
                                [--chrome /path/to/chrome]

--preview also writes the icon as the home screen would show it: masked to a
rounded square on a light background.
"""
import argparse
import glob
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

SIZE = 1024


# --- the letter --------------------------------------------------------------

def letter_n(cx, top, height):
    """The outline of a semibold N, as a list of points: two vertical stems
    and a heavier diagonal from the top of the left stem to the foot of the
    right one."""
    width = 0.74 * height          # glyph width
    stem = 0.125 * height          # vertical stems
    diag = 0.170 * height          # horizontal width of the diagonal band
    left, right = cx - width / 2, cx + width / 2
    bottom = top + height
    # Where the diagonal's edges meet the stems' inner edges.
    y_right = top + (right - stem - (left + diag)) * height / (right - (left + diag))
    y_left = top + stem * height / (right - diag - left)
    pts = [(left, bottom), (left, top), (left + diag, top), (right - stem, y_right),
           (right - stem, top), (right, top), (right, bottom), (right - diag, bottom),
           (left + stem, y_left), (left + stem, bottom)]
    return pts


def path_of(pts):
    return "M" + " L".join("%.1f,%.1f" % p for p in pts) + " Z"


def inside(pts, x, y):
    """Even-odd point-in-polygon test."""
    hit = False
    for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]):
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit


# --- the droplets -----------------------------------------------------------

def droplets(rng):
    """(x, y, r) for the water on the glass: many small drops, some medium,
    and a few large rings clustered at the top left, as on the iOS 6 art."""
    drops = [(215, 150, 44), (318, 196, 33), (182, 262, 40), (140, 118, 20)]
    for _ in range(170):
        r = rng.choice([4, 5, 6, 7, 8, 9, 10, 12, 14, 17, 21])
        drops.append((rng.uniform(20, SIZE - 20), rng.uniform(20, SIZE - 20), r))
    # Keep drops from sitting inside one another.
    kept = []
    for d in drops:
        if all(math.hypot(d[0] - k[0], d[1] - k[1]) > d[2] + k[2] + 3 for k in kept):
            kept.append(d)
    return kept


def droplet_svg(x, y, r, on_dark):
    """One drop: a faint body, a rim darkest on its upper side (the drop
    refracts the darker glass above it), and a bright highlight low right."""
    rim = max(1.2, 0.16 * r)
    parts = [
        '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="url(#dropBody)"/>' % (x, y, r),
        '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="url(#%s)" '
        'stroke-width="%.1f"/>' % (x, y, r - rim / 2, "rimLight" if on_dark else "rimDark", rim),
        '<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="white" '
        'fill-opacity="0.85"/>' % (x + 0.30 * r, y + 0.42 * r, 0.30 * r, 0.20 * r),
    ]
    if r >= 12:  # a second, softer glint on the bigger drops
        parts.append('<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="white" '
                     'fill-opacity="0.45"/>' % (x - 0.38 * r, y - 0.40 * r, 0.14 * r, 0.09 * r))
    return "\n".join(parts)


# --- the whole icon ---------------------------------------------------------

def icon_svg(seed=6):
    rng = random.Random(seed)
    glyph_top, glyph_h = 205, 614
    pts = letter_n(SIZE / 2, glyph_top, glyph_h)
    n_path = path_of(pts)

    # The letter is behind the wet glass, so every drop is drawn in front of
    # it; one centred on the ink gets the light rim that shows on black, and
    # the ink carries about half the drops the bare glass does.
    drops = []
    for (x, y, r) in droplets(rng):
        on_ink = inside(pts, x, y)
        if on_ink and rng.random() < 0.5:
            continue
        drops.append(droplet_svg(x, y, r, on_ink))

    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">
<defs>
  <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0"    stop-color="#7fd6ef"/>
    <stop offset="0.28" stop-color="#a6dcec"/>
    <stop offset="0.42" stop-color="#c3d6dc"/>
    <stop offset="0.47" stop-color="#cfd2d5"/>
    <stop offset="1"    stop-color="#d9dadc"/>
  </linearGradient>
  <linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0"    stop-color="white" stop-opacity="0"/>
    <stop offset="0.40" stop-color="white" stop-opacity="0.0"/>
    <stop offset="0.45" stop-color="white" stop-opacity="0.35"/>
    <stop offset="0.52" stop-color="white" stop-opacity="0"/>
  </linearGradient>
  <linearGradient id="ink" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0"    stop-color="#7a7c7e"/>
    <stop offset="0.35" stop-color="#4b4c4e"/>
    <stop offset="0.75" stop-color="#1c1c1d"/>
    <stop offset="1"    stop-color="#080808"/>
  </linearGradient>
  <linearGradient id="inkEdge" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0"    stop-color="white" stop-opacity="0.22"/>
    <stop offset="0.5"  stop-color="white" stop-opacity="0.04"/>
    <stop offset="1"    stop-color="white" stop-opacity="0"/>
  </linearGradient>
  <radialGradient id="dropBody" cx="0.5" cy="0.45" r="0.55">
    <stop offset="0"   stop-color="white" stop-opacity="0.18"/>
    <stop offset="0.8" stop-color="white" stop-opacity="0.05"/>
    <stop offset="1"   stop-color="#0c3040" stop-opacity="0.18"/>
  </radialGradient>
  <linearGradient id="rimDark" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0"    stop-color="#0d3444" stop-opacity="0.75"/>
    <stop offset="0.55" stop-color="#0d3444" stop-opacity="0.25"/>
    <stop offset="1"    stop-color="white"   stop-opacity="0.55"/>
  </linearGradient>
  <linearGradient id="rimLight" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0"    stop-color="white" stop-opacity="0.10"/>
    <stop offset="1"    stop-color="white" stop-opacity="0.45"/>
  </linearGradient>
  <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
    <feDropShadow dx="0" dy="8" stdDeviation="9" flood-color="#000" flood-opacity="0.35"/>
  </filter>
</defs>
<rect width="{SIZE}" height="{SIZE}" fill="url(#glass)"/>
<rect width="{SIZE}" height="{SIZE}" fill="url(#sheen)"/>
<path d="{n_path}" fill="url(#ink)" filter="url(#shadow)"/>
<path d="{n_path}" fill="none" stroke="url(#inkEdge)" stroke-width="2.5"/>
{chr(10).join(drops)}
</svg>
"""


# --- rendering --------------------------------------------------------------

def find_chrome(explicit):
    if explicit:
        return explicit
    for pattern in ("/opt/pw-browsers/chromium-*/chrome-linux/chrome",):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]
    for name in ("chromium", "chromium-browser", "google-chrome", "chrome"):
        path = shutil.which(name)
        if path:
            return path
    sys.exit("no Chromium found; pass --chrome")


def render(chrome, html, out_png, width, height):
    """Screenshot the page, cropped to width x height, alpha dropped.
    Headless Chromium keeps part of the window for itself, so the window is
    made taller than the page and the shot cropped back."""
    from PIL import Image
    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "icon.html")
        with open(page, "w") as f:
            f.write(html)
        subprocess.run([chrome, "--headless=new", "--no-sandbox", "--disable-gpu",
                        "--hide-scrollbars", "--force-device-scale-factor=1",
                        f"--window-size={width},{height + 200}",
                        f"--screenshot={out_png}", "file://" + page],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    Image.open(out_png).convert("RGB").crop((0, 0, width, height)).save(out_png, optimize=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="app/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")
    ap.add_argument("--preview")
    ap.add_argument("--svg")
    ap.add_argument("--chrome")
    args = ap.parse_args()

    svg = icon_svg()
    if args.svg:
        with open(args.svg, "w") as f:
            f.write(svg)
    chrome = find_chrome(args.chrome)
    page = "<html><body style='margin:0'>%s</body></html>" % svg
    render(chrome, page, args.out, SIZE, SIZE)
    print("wrote", args.out)

    if args.preview:
        radius = 0.2237 * 600   # the iOS app-icon corner, at 600 px
        preview = f"""<html><body style="margin:0;background:linear-gradient(#f4f4f4,#dcdcdc);
display:flex;align-items:center;justify-content:center;height:760px">
<div style="width:600px;height:600px;border-radius:{radius:.0f}px;overflow:hidden;
box-shadow:0 18px 40px rgba(0,0,0,.28)">
<div style="transform:scale({600 / SIZE});transform-origin:0 0">{svg}</div></div>
</body></html>"""
        render(chrome, preview, args.preview, 760, 760)
        print("wrote", args.preview)


if __name__ == "__main__":
    main()
