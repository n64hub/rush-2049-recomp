"""Make the launcher's artwork and the program's icon.

Usage: .venv/Scripts/python -I tools/launcher_art.py [--chrome <chrome or chromium>]
Needs fontTools and Pillow in the venv.

All of it is drawn here, from shapes and the menus' own font (assets/InterVariable.ttf, SIL OFL 1.1), so none of it
comes from the game or its box: a night San Francisco with a suspension bridge, a skyline and a road into it, a
wordmark set in Inter Black, and an "R" on the same night for the icon. It writes

    assets/launcher/background.svg   the launcher's background (src/main.cpp, on_launcher_init)
    assets/launcher/title.svg        the wordmark in place of RecompFrontend's text title
    icon/icon.svg                    the icon, and with --chrome, rasterised by a headless Chrome into
    icon/rush2049.ico                the executable's and window's icon on Windows (icon/rush2049.rc)
    icon/rush2049.bmp                the window's icon elsewhere (SDL_SetWindowIcon in src/main.cpp)

The background is 1600x1200, the window's default 4:3, and the launcher stretches it to the window's width and
centres it: a wider window crops its top and bottom, a narrower one shows the launcher's own background colour above
and below it, so both edges are that colour (RecompFrontend's Background1, #020712). The menu sits in the middle of
the window, over the skyline's dark silhouettes and the road, and the wordmark above it in the sky.
"""
import argparse
import io
import os
import random
import shutil
import subprocess
import sys
import tempfile

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "assets", "InterVariable.ttf")
BG = "#020712"


def load_font(weight):
    return instancer.instantiateVariableFont(TTFont(FONT), {"wght": weight, "opsz": 32})


def text_path(font, text, size, x, y, tracking=0.0, anchor="middle"):
    """SVG path data for text at baseline y, size in pixels, tracking in ems; returns (d, width)."""
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    scale = size / font["head"].unitsPerEm
    names = [cmap[ord(c)] for c in text]
    advances = [font["hmtx"][n][0] * scale + tracking * size for n in names]
    width = sum(advances) - tracking * size
    pen_x = x - {"start": 0, "middle": width / 2, "end": width}[anchor]
    pen = SVGPathPen(glyphs, ntos=lambda v: f"{v:.1f}".rstrip("0").rstrip("."))
    for name, advance in zip(names, advances):
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, pen_x, y)))
        pen_x += advance
    return pen.getCommands(), width


def background():
    rnd = random.Random(2049)
    w, h, horizon = 1600, 1200, 740
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">', "<defs>",
           '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">'
           f'<stop offset="0" stop-color="{BG}"/><stop offset="0.3" stop-color="#0a0b2e"/>'
           '<stop offset="0.5" stop-color="#2a0f4f"/><stop offset="0.57" stop-color="#7a1a6a"/>'
           '<stop offset="0.617" stop-color="#ff5a3c"/><stop offset="0.617" stop-color="#140826"/>'
           f'<stop offset="0.8" stop-color="#07061a"/><stop offset="1" stop-color="{BG}"/></linearGradient>',
           f'<radialGradient id="glow" cx="800" cy="{horizon}" r="700" gradientUnits="userSpaceOnUse" '
           'gradientTransform="translate(0 740) scale(1 0.32) translate(0 -740)">'
           '<stop offset="0" stop-color="#ff7a3c" stop-opacity="0.75"/><stop offset="0.45" stop-color="#d42a6e" '
           'stop-opacity="0.35"/><stop offset="1" stop-color="#d42a6e" stop-opacity="0"/></radialGradient>',
           '<linearGradient id="city" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1c0f3a"/>'
           '<stop offset="1" stop-color="#0a0718"/></linearGradient>',
           '<linearGradient id="road" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1a0d33"/>'
           f'<stop offset="1" stop-color="{BG}"/></linearGradient>',
           '<linearGradient id="fade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ff3fa4"/>'
           '<stop offset="1" stop-color="#ff3fa4" stop-opacity="0"/></linearGradient>',
           '<linearGradient id="fade2" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3fe0ff"/>'
           '<stop offset="1" stop-color="#3fe0ff" stop-opacity="0"/></linearGradient>',
           "</defs>",
           f'<rect width="{w}" height="{h}" fill="url(#sky)"/>']

    # Stars, thinning toward the horizon.
    for _ in range(170):
        x, y = rnd.uniform(0, w), rnd.uniform(0, 1) ** 1.6 * 560
        out.append(f'<circle cx="{x:.0f}" cy="{y:.0f}" r="{rnd.uniform(0.8, 2.2):.1f}" fill="#cfd8ff" '
                   f'opacity="{rnd.uniform(0.25, 0.9) * (1 - y / 700):.2f}"/>')

    # Light trails of flying cars, long thin arcs across the sky.
    for y0, y1, color, op in ((560, 500, "#3fe0ff", 0.55), (520, 590, "#ff3fa4", 0.45), (600, 640, "#ffb238", 0.4)):
        out.append(f'<path d="M -50 {y0} Q 800 {(y0 + y1) / 2 - 90} 1650 {y1}" fill="none" stroke="{color}" '
                   f'stroke-width="2.5" stroke-linecap="round" opacity="{op}"/>')

    out.append(f'<rect x="0" y="{horizon - 260}" width="{w}" height="520" fill="url(#glow)"/>')

    # The skyline: blocks with lit windows, a tapering pyramid tower, and spires.
    def tower(x, top, width, color="url(#city)"):
        out.append(f'<rect x="{x:.0f}" y="{top:.0f}" width="{width:.0f}" height="{horizon - top + 1:.0f}" '
                   f'fill="{color}"/>')
        for wy in range(int(top) + 12, horizon - 8, 14):
            for wx in range(int(x) + 6, int(x + width) - 6, 10):
                if rnd.random() < 0.22:
                    lit = rnd.choice(("#ffd27a", "#7ae8ff", "#ff9ad5"))
                    out.append(f'<rect x="{wx}" y="{wy}" width="4" height="6" fill="{lit}" '
                               f'opacity="{rnd.uniform(0.35, 0.85):.2f}"/>')

    x = 520
    while x < 1640:
        width = rnd.uniform(40, 90)
        if not 1130 < x < 1250:
            tower(x, horizon - rnd.uniform(70, 230) * (1.2 if 700 < x < 1000 else 1.0), width)
        x += width + rnd.uniform(-10, 12)
    out.append(f'<path d="M 1190 {horizon - 420} L 1232 {horizon} L 1148 {horizon} Z" fill="#20113f"/>')
    out.append(f'<path d="M 1190 {horizon - 470} L 1190 {horizon - 420}" stroke="#ff3fa4" stroke-width="3"/>')
    out.append(f'<circle cx="1190" cy="{horizon - 472}" r="4" fill="#ff3fa4"/>')
    for x, top in ((900, 170), (650, 140)):
        out.append(f'<path d="M {x} {horizon - top - 60} L {x + 8} {horizon - top} L {x - 8} {horizon - top} Z" '
                   'fill="#1c0f3a"/>')
        out.append(f'<circle cx="{x}" cy="{horizon - top - 62}" r="3" fill="#3fe0ff"/>')
    for x in range(-20, 520, 46):
        tower(x, horizon - rnd.uniform(30, 80), rnd.uniform(30, 50), "#120a28")

    # A suspension bridge on the left, its deck at the horizon, in international orange.
    deck, towers, top = horizon - 34, (130, 430), horizon - 330
    span = towers[1] - towers[0]
    cable = (f"M -40 {deck - 20} Q {towers[0] - 60} {deck - 60} {towers[0]} {top + 14} "
             f"Q {towers[0] + span / 2} {deck + 70} {towers[1]} {top + 14} Q {towers[1] + 120} {deck - 80} 560 {deck}")
    out.append(f'<path d="{cable}" fill="none" stroke="#ff5a2e" stroke-width="5"/>')
    for i in range(1, 16):
        hx = towers[0] + span * i / 16
        t = i / 16
        hy = (1 - t) ** 2 * (top + 14) + 2 * t * (1 - t) * (deck + 70) + t ** 2 * (top + 14)
        out.append(f'<path d="M {hx:.0f} {hy:.0f} L {hx:.0f} {deck}" stroke="#ff5a2e" stroke-width="1.5" '
                   'opacity="0.8"/>')
    for tx in towers:
        out.append(f'<path d="M {tx - 16} {horizon + 4} L {tx - 11} {top} L {tx + 11} {top} L {tx + 16} '
                   f'{horizon + 4} L {tx + 7} {horizon + 4} L {tx + 5} {top + 20} L {tx - 5} {top + 20} '
                   f'L {tx - 7} {horizon + 4} Z" fill="#e8442a"/>')
        for by in (top + 40, top + 120, top + 200):
            out.append(f'<rect x="{tx - 12}" y="{by}" width="24" height="9" fill="#e8442a"/>')
        out.append(f'<circle cx="{tx}" cy="{top - 6}" r="4" fill="#ff3f3f"/>')
    out.append(f'<rect x="-40" y="{deck}" width="620" height="9" fill="#c83a26"/>')
    out.append(f'<path d="M -40 {deck + 4} L 580 {deck + 4}" stroke="#ffd27a" stroke-width="2" '
               'stroke-dasharray="3 9" opacity="0.8"/>')

    # The road, from the horizon to the bottom edge, with neon edges and a dashed centre line.
    vx = 800
    out.append(f'<path d="M {vx - 30} {horizon} L {vx + 30} {horizon} L 1900 {h} L -300 {h} Z" fill="url(#road)"/>')
    out.append(f'<path d="M {vx - 30} {horizon} L -300 {h}" stroke="url(#fade)" stroke-width="5"/>')
    out.append(f'<path d="M {vx + 30} {horizon} L 1900 {h}" stroke="url(#fade2)" stroke-width="5"/>')
    z = 1.0
    while z < 40:
        y0, y1 = horizon + (h - horizon) / z, horizon + (h - horizon) / (z + 0.5)
        op = min(1.0, 2.5 / z) * 0.8
        out.append(f'<path d="M {vx} {y0:.1f} L {vx} {y1:.1f}" stroke="#ffd27a" stroke-width="{10 / z + 1:.1f}" '
                   f'opacity="{op:.2f}"/>')
        z += 1.2
    # Ground lines across, closer together toward the horizon.
    z = 1.0
    while z < 30:
        y = horizon + (h - horizon) / z
        out.append(f'<path d="M 0 {y:.1f} L {w} {y:.1f}" stroke="#7a1a6a" stroke-width="1.5" '
                   f'opacity="{0.35 / z ** 0.5:.2f}"/>')
        z *= 1.6
    out.append("</svg>")
    return "\n".join(out) + "\n"


def title():
    black, bold = load_font(900), load_font(650)
    # RUSH 2049 sized to 880 of the 1000 wide, its cap height then ~0.73 of the size above the baseline at 236.
    _, natural = text_path(black, "RUSH 2049", 100, 0, 0, tracking=-0.01)
    size = 100 * 880 / natural
    rush, _ = text_path(black, "RUSH 2049", size, 500, 236, tracking=-0.01)
    city, _ = text_path(bold, "SAN FRANCISCO", 46, 500, 66, tracking=0.42)
    sub, _ = text_path(bold, "RECOMPILED", 40, 500, 318, tracking=0.6)
    top = 236 - 0.73 * size
    # Skewed about the baseline, so the letters lean right without moving along it.
    skew = 'transform="translate(0 236) skewX(-12) translate(0 -236)"'
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 340" width="1000" height="340">', "<defs>",
           f'<linearGradient id="chrome" x1="0" y1="{top:.0f}" x2="0" y2="236" gradientUnits="userSpaceOnUse">'
           '<stop offset="0" stop-color="#fff6d0"/><stop offset="0.42" stop-color="#ffc23a"/>'
           '<stop offset="0.5" stop-color="#ff7a1c"/><stop offset="0.52" stop-color="#ff5a1f"/>'
           '<stop offset="1" stop-color="#b8121e"/></linearGradient>',
           "</defs>",
           f'<path d="{rush}" fill="#3fe0ff" opacity="0.55" '
           'transform="translate(9 7) translate(0 236) skewX(-12) translate(0 -236)"/>',
           f'<path d="{rush}" fill="none" stroke="#14051f" stroke-width="12" stroke-linejoin="round" {skew}/>',
           f'<path d="{rush}" fill="url(#chrome)" {skew}/>',
           f'<path d="{city}" fill="#eef1ff"/>',
           f'<path d="{sub}" fill="#3fe0ff"/>',
           "</svg>"]
    return "\n".join(out) + "\n"


def icon():
    black = load_font(900)
    r, _ = text_path(black, "R", 200, 134, 200)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
<defs>
<linearGradient id="night" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0a0b2e"/>
<stop offset="0.62" stop-color="#7a1a6a"/><stop offset="0.7" stop-color="#ff5a3c"/>
<stop offset="0.7" stop-color="#140826"/><stop offset="1" stop-color="{BG}"/></linearGradient>
<linearGradient id="chrome" x1="0" y1="56" x2="0" y2="200" gradientUnits="userSpaceOnUse">
<stop offset="0" stop-color="#fff6d0"/><stop offset="0.42" stop-color="#ffc23a"/><stop offset="0.5" stop-color="#ff7a1c"/>
<stop offset="1" stop-color="#b8121e"/></linearGradient>
<clipPath id="tile"><rect x="8" y="8" width="240" height="240" rx="44"/></clipPath>
</defs>
<g clip-path="url(#tile)">
<rect width="256" height="256" fill="url(#night)"/>
<path d="M 120 179 L 136 179 L 300 256 L -44 256 Z" fill="#1a0d33"/>
<path d="M 120 179 L -44 256" stroke="#ff3fa4" stroke-width="7"/>
<path d="M 136 179 L 300 256" stroke="#3fe0ff" stroke-width="7"/>
</g>
<path d="{r}" fill="none" stroke="#14051f" stroke-width="16" stroke-linejoin="round" transform="translate(30 0) skewX(-12)"/>
<path d="{r}" fill="url(#chrome)" transform="translate(30 0) skewX(-12)"/>
<rect x="8" y="8" width="240" height="240" rx="44" fill="none" stroke="#14051f" stroke-width="4"/>
</svg>
"""


def rasterise(chrome, svg_path, size):
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, "icon.png")
        page = os.path.join(tmp, "icon.html")
        with open(page, "w") as f:
            f.write(f'<html><body style="margin:0"><img src="file:///{svg_path.replace(os.sep, "/")}" '
                    f'width="{size}" height="{size}"></body></html>')
        subprocess.run([chrome, "--headless", "--disable-gpu", "--hide-scrollbars", "--mute-audio",
                        f"--user-data-dir={os.path.join(tmp, 'profile')}", "--default-background-color=00000000",
                        f"--window-size={size},{size}", f"--screenshot={png}", "file:///" + page.replace(os.sep, "/")],
                       check=True, capture_output=True)
        return Image.open(png).convert("RGBA").copy()


def write(path, text):
    with open(os.path.join(ROOT, path), "w", newline="\n") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--chrome", help="a Chrome or Chromium to rasterise the icon with")
    args = parser.parse_args()
    os.makedirs(os.path.join(ROOT, "assets", "launcher"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "icon"), exist_ok=True)
    write("assets/launcher/background.svg", background())
    write("assets/launcher/title.svg", title())
    write("icon/icon.svg", icon())
    if args.chrome:
        svg = os.path.join(ROOT, "icon", "icon.svg")
        big = rasterise(args.chrome, svg, 256)
        big.save(os.path.join(ROOT, "icon", "rush2049.ico"), sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
        # SDL reads a 32-bit BMP's fourth byte as alpha.
        big.resize((128, 128), Image.LANCZOS).save(os.path.join(ROOT, "icon", "rush2049.bmp"))


if __name__ == "__main__":
    main()
