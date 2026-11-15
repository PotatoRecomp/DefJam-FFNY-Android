#!/usr/bin/env python3
"""The on-screen controls' art: every control as a PNG, normal and held, into app/src/main/res/drawable-nodpi, drawn
by TouchControls.java. Each is drawn here as an SVG and rendered by rsvg-convert.

Minimal, as a mobile game's controls are: a flat see-through dark disc with one thin white rim, the action as a clean
white outline icon (a boxing glove to punch, a sneaker to kick, an open hand to grab, a dash to run, a shield to block,
a bolt for hard), every line of one weight with round ends and closed shapes only faintly filled, and the pad's button
small and faint at the bottom. Held, the disc lightens. The icons are drawn here from plain paths. Every text is
measured as rendered and set smaller until it fits.
"""
import math
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(os.environ.get("CONTROLS_OUT", ROOT / "app" / "src" / "main" / "res" / "drawable-nodpi"))
PX = 256          # the PNG's side
C, R = 130, 100   # the control's centre and radius in the box
TEXT = "Noto Sans"
ICON_UP = -8      # the icon sits a little high, the letter under it


def svg(body):
    return f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 260 260' width='{PX}' height='{PX}'>{body}</svg>"


# ---------------------------------------------------------------- measuring text as rendered

_boxes = {}


def measure(body):
    """The box (x, y, w, h) of what an SVG body draws, in the 260 box's units."""
    if body not in _boxes:
        with tempfile.TemporaryDirectory() as tmp:
            src, png = Path(tmp) / "t.svg", Path(tmp) / "t.png"
            src.write_text(svg(body).replace(f"width='{PX}' height='{PX}'", "width='260' height='260'"))
            subprocess.run(["rsvg-convert", str(src), "-o", str(png)], check=True)
            box = subprocess.run(["magick", str(png), "-alpha", "extract", "-threshold", "8%", "-format", "%@",
                                  "info:"], check=True, capture_output=True, text=True).stdout
        w, rest = box.split("x")
        h, x, y = (int(v) for v in rest.split("+"))
        _boxes[body] = (x, y, int(w), h)
    return _boxes[body]


def text(s, x, y, size, opacity):
    return (f"<text x='{x}' y='{y}' font-size='{size}' font-family='{TEXT}' font-weight='800' text-anchor='middle' "
            f"dominant-baseline='central' fill='#ffffff' fill-opacity='{opacity}'>{s}</text>")


def fit_circle(make, size, r, smallest=10):
    """make(size) at the largest size up to size whose drawn box lies inside a circle of radius r about the centre."""
    while size > smallest:
        x, y, w, h = measure(make(size))
        if all((px - C) ** 2 + (py - C) ** 2 <= r * r for px in (x, x + w) for py in (y, y + h)):
            break
        size -= 1
    return make(size)


# ---------------------------------------------------------------- the icons: outlines of one weight, faint inside

W = 7  # the outlines' weight, every icon's


def shape(d):
    """A closed outline, faintly filled so it holds together over a busy fight."""
    return f"<path d='{d}' fill='#ffffff' fill-opacity='0.16'/>"


def line(d, w=W):
    """An open line: the strokes inside a shape, and the icons that are only lines."""
    return f"<path d='{d}' fill='none' stroke-width='{w}'/>"


def glove():
    """Punch: a boxing glove standing up, as it is always drawn: the round mitt, the thumb's lobe on its side, the cuff
    under it."""
    # the mitt and the thumb's lobe as one outline, so no line ends where they meet; the thumb's seam runs between the
    # two points on the mitt's side where the lobe leaves and rejoins it
    return (shape("M104 150 L103.62 146 Q78 150 76 130 Q76 108 100.38 112 L100 108 Q98 70 140 70 Q182 70 180 110 "
                  "L176 142 Q175 150 166 150 Z") +
            line("M100.38 112 L103.62 146") +
            shape("M110 150 L168 150 L168 182 Q168 188 162 188 L116 188 Q110 188 110 182 Z") +    # the cuff
            line("M124 169 L154 169"))                                                           # its lacing


def sneaker():
    """Kick: a sneaker side on, toe to the right, the sole along its bottom and two laces."""
    return (shape("M78 166 L78 102 Q78 92 88 92 L114 92 Q124 92 126 100 L130 116 Q164 118 184 130 Q198 138 198 152 "
                  "L198 158 Q198 166 190 166 Z") +
            line("M78 150 L198 150") +       # the sole
            line("M104 112 L116 108") + line("M108 128 L122 124"))  # the laces


def hand():
    """Grab: an open hand, palm on: four fingers sharing their sides, the thumb curling in from the left."""
    # the index finger's side starts where the thumb's inner side meets it, (88, 137), so nothing sticks out below
    return (line("M88 137 L88 96 A11 11 0 0 1 110 96 L110 124") +
            line("M110 116 L110 84 A11 11 0 0 1 132 84 L132 116") +
            line("M132 116 L132 88 A11 11 0 0 1 154 88 L154 120") +
            line("M154 122 L154 104 A11 11 0 0 1 176 104 L176 150 Q176 190 138 190 L126 190 Q106 190 94 176 "
                 "L70 150 A11 11 0 0 1 86.2 135.1 L88 137"))


def dash():
    """Run: a double chevron, onwards."""
    return line("M94 92 L130 130 L94 168", 10) + line("M136 92 L172 130 L136 168", 10)


def shield():
    """Block: a shield, a line down its middle."""
    return shape("M130 72 L180 90 L180 126 Q180 168 130 188 Q80 168 80 126 L80 90 Z") + line("M130 98 L130 162")


def bolt_path(cx=C, cy=C, s=1.0):
    pts = [(14, -58), (-30, 8), (-2, 8), (-14, 58), (30, -10), (2, -10)]
    return "M" + " L".join(f"{cx + x * s:.1f} {cy + y * s:.1f}" for x, y in pts) + " Z"


def bolt():
    """Hard: a lightning bolt."""
    return shape(bolt_path())


def crosshair():
    """Target: a ring, its four marks and a dot."""
    out = f"<circle cx='{C}' cy='{C}' r='36' fill='none' stroke-width='{W}'/>"
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        out += line(f"M{C + dx * 22} {C + dy * 22} L{C + dx * 56} {C + dy * 56}")
    return out + f"<circle cx='{C}' cy='{C}' r='6' fill='#ffffff' stroke='none'/>"


def pause():
    return line("M112 96 L112 164", 12) + line("M148 96 L148 164", 12)


def back():
    """Back: an arrow to the left."""
    return line("M88 130 L172 130", 10) + line("M118 98 L86 130 L118 162", 10)


def cog():
    """Settings: a cog's outline and its hole."""
    pts = []
    step = 2 * math.pi / 8
    outer, inner = 54, 40
    for i in range(8):
        for k, rr in ((-0.26, inner), (-0.14, outer), (0.14, outer), (0.26, inner)):
            a = i * step + k * step
            pts.append(f"{C + math.cos(a) * rr:.1f} {C + math.sin(a) * rr:.1f}")
    return shape("M" + " L".join(pts) + " Z") + f"<circle cx='{C}' cy='{C}' r='15' fill='none' stroke-width='{W}'/>"


def arrow_icon(dx, dy):
    """A d-pad arrow: a chevron pointing its way."""
    t = 26
    a = (C - dx * t * 0.5 - dy * t * 1.1, C - dy * t * 0.5 + dx * t * 1.1)
    tip = (C + dx * t * 0.7, C + dy * t * 0.7)
    b = (C - dx * t * 0.5 + dy * t * 1.1, C - dy * t * 0.5 - dx * t * 1.1)
    return lambda: line(f"M{a[0]:.1f} {a[1]:.1f} L{tip[0]:.1f} {tip[1]:.1f} L{b[0]:.1f} {b[1]:.1f}", 11)


# ---------------------------------------------------------------- the button

def disc(on, r=R):
    return (f"<circle cx='{C}' cy='{C}' r='{r - 2}' fill='{'#ffffff' if on else '#000000'}' "
            f"fill-opacity='{0.28 if on else 0.32}'/>"
            f"<circle cx='{C}' cy='{C}' r='{r - 2.5}' fill='none' stroke='#ffffff' "
            f"stroke-opacity='{0.95 if on else 0.6}' stroke-width='{4 if on else 3}'/>")


def icon(draw, s=1.0, dx=0.0, dy=0.0):
    """The icon in white lines, scaled and moved about the centre; the weight stays the same at any scale."""
    return (f"<g transform='translate({C + dx} {C + dy}) scale({s}) translate({-C} {-C})' stroke='#ffffff' "
            f"stroke-opacity='0.95' stroke-width='{W / s:.2f}' stroke-linecap='round' stroke-linejoin='round'>"
            f"{draw()}</g>")


def letter(label):
    """The pad's button, small and faint at the bottom of the disc."""
    return fit_circle(lambda n: text(label, C, C + 68, n, 0.6), 24, R - 10)


def button(draw, label, on, hard=False):
    body = disc(on)
    if hard:
        body += icon(draw, 0.76, -6, ICON_UP + 2)
        body += (f"<path d='{bolt_path(C + 48, C - 44, 0.34)}' fill='#ffffff' fill-opacity='0.95' stroke='#ffffff' "
                 f"stroke-width='2' stroke-linejoin='round'/>")
    else:
        body += icon(draw, 0.86 if label else 1.0, 0, ICON_UP if label else 0)
    if label:
        body += letter(label)
    return svg(body)


def empty():
    return svg("")


def stick_base():
    """The stick's well: the disc and its rim, a faint inner ring where the thumb travels."""
    return svg(disc(False) + f"<circle cx='{C}' cy='{C}' r='{R * 0.6}' fill='none' stroke='#ffffff' "
                             f"stroke-opacity='0.25' stroke-width='2.5'/>")


def knob(on):
    """The thumb: a light disc with the white rim."""
    r = R * 0.94
    return svg(f"<circle cx='{C}' cy='{C}' r='{r - 2}' fill='#ffffff' fill-opacity='{0.45 if on else 0.28}' "
               f"stroke='#ffffff' stroke-opacity='0.9' stroke-width='3'/>")


# the silhouette, and the pad's button under it ("" none); the macros are marked hard
BUTTONS = {
    "y": (glove, "Y"),
    "x": (sneaker, "X"),
    "a": (hand, "A"),
    "b": (dash, "B"),
    "lt": (bolt, "LT"),
    "rt": (shield, "RT"),
    "white": (crosshair, "W"),
    "start": (pause, ""),
    "back": (back, ""),
    "gear": (cog, ""),
}
MACROS = {
    "hard_punch": (glove, "LT+Y"),
    "hard_kick": (sneaker, "LT+X"),
    "hard_grab": (hand, "LT+A"),
}


def art():
    out = {}
    for on in (False, True):
        s = "_on" if on else ""
        for key, (draw, label) in BUTTONS.items():
            out[f"ctl_{key}{s}"] = button(draw, label, on)
        for key, (draw, label) in MACROS.items():
            out[f"ctl_{key}{s}"] = button(draw, label, on, hard=True)
        # Unused buttons in Def Jam (L3, R3, Black) are blanked:
        for unused in ("l3", "r3", "black"):
            out[f"ctl_{unused}{s}"] = empty()
        for n, d in (("up", (0, -1)), ("down", (0, 1)), ("left", (-1, 0)), ("right", (1, 0))):
            out[f"ctl_arrow_{n}{s}"] = button(arrow_icon(*d), "", on)
        out[f"ctl_knob{s}"] = knob(on)
    out["ctl_stick"] = stick_base()
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    images = art()
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in images.items():
            src = Path(tmp) / f"{name}.svg"
            src.write_text(body)
            subprocess.run(["rsvg-convert", str(src), "-w", str(PX), "-h", str(PX), "-o", str(OUT / f"{name}.png")],
                           check=True)
    print(f"{OUT}: {len(images)} images")


if __name__ == "__main__":
    main()
