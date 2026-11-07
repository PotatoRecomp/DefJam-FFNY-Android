#!/usr/bin/env python3
"""The on-screen controls' art: every control as a PNG, normal and held, into app/src/main/res/drawable-nodpi, drawn
by TouchControls.java. Each is drawn here as an SVG and rendered by rsvg-convert.

The look is a phone game's, not the pad's: see-through round controls, clear in the middle, a soft white glow inside
the edge that raises them and a thin white rim, no drop shadow. The face buttons carry the Xbox pad's letters, thin
and uncoloured, the rest their names; a held control is the same brighter.

Every image is a control of radius 100 in a 260 x 260 box centred on it, so TouchControls draws each one 1.3 times its
radius out from its centre. Run this after changing the look or a label and commit the PNGs. Needs rsvg-convert and
Noto Sans."""
import math
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(os.environ.get("CONTROLS_OUT", ROOT / "app" / "src" / "main" / "res" / "drawable-nodpi"))
PX = 256          # the PNG's side
C, R = 130, 100   # the control's centre and radius in the box

GLYPH = "#e6e6e6"  # a soft white: pure white stood out over the game
FONT = "font-family='Noto Sans' font-weight='800' text-anchor='middle' dominant-baseline='central'"

DEFS = """<defs>
<radialGradient id='body' cx='50%' cy='50%' r='50%'>
  <stop offset='0' stop-color='#fff' stop-opacity='0.03'/>
  <stop offset='0.62' stop-color='#fff' stop-opacity='0.03'/>
  <stop offset='1' stop-color='#fff' stop-opacity='0.10'/>
</radialGradient>
<radialGradient id='bodyon' cx='50%' cy='50%' r='50%'>
  <stop offset='0' stop-color='#fff' stop-opacity='0.10'/>
  <stop offset='0.62' stop-color='#fff' stop-opacity='0.14'/>
  <stop offset='1' stop-color='#fff' stop-opacity='0.24'/>
</radialGradient>
<filter id='soft' x='-20%' y='-20%' width='140%' height='140%'><feGaussianBlur stdDeviation='5'/></filter>
</defs>"""



def svg(body):
    return (f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 260 260' width='{PX}' height='{PX}'>"
            f"{DEFS}{body}</svg>")


def surface(shape, on, glow=None, clip=R):
    """A control's body: clear in the middle and whiter towards its edge, a wide soft white glow inside the edge that
    raises it (blurred, kept inside the control), and a thin white rim; held, all of it brighter. No drop shadow.
    shape is an SVG element with {style} where its attributes go; glow the ring the soft glow is drawn along."""
    out = shape.format(style=f"fill='url(#{'bodyon' if on else 'body'})'")
    if glow:
        # kept inside the control's own edge
        out += (f"<clipPath id='in{clip:.0f}'><circle cx='{C}' cy='{C}' r='{clip}'/></clipPath>"
                f"<g clip-path='url(#in{clip:.0f})'>" +
                glow.format(style=f"fill='none' stroke='#fff' stroke-opacity='{0.35 if on else 0.18}' "
                                  f"stroke-width='10' "
                                  f"filter='url(#soft)'") + "</g>")
    out += shape.format(style=f"fill='none' stroke='#fff' stroke-opacity='{0.65 if on else 0.4}' stroke-width='2.5'")
    return out


def disc(on, r=R):
    return surface(f"<circle cx='{C}' cy='{C}' r='{r - 1.5}' {{style}}/>", on,
                   f"<circle cx='{C}' cy='{C}' r='{r - 4}' {{style}}/>", r)


def text(s, size, dy=0, spacing=0):
    return (f"<text x='{C}' y='{C + dy}' font-size='{size}' letter-spacing='{spacing}' fill='{GLYPH}' stroke='#000' "
            f"stroke-opacity='0.30' stroke-width='4' paint-order='stroke' {FONT}>{s}</text>")


def letter(ch, on):
    """A face button: its letter large and thin, in the glyphs' soft white."""
    return svg(disc(on) +
               f"<text x='{C}' y='{C + 4}' font-size='{R * 1.0:.0f}' font-family='Noto Sans' font-weight='400' "
               f"text-anchor='middle' dominant-baseline='central' fill='{GLYPH}' stroke='#000' stroke-opacity='0.25' "
               f"stroke-width='2' paint-order='stroke'>{ch}</text>")


def label(name, size, on):
    return svg(disc(on) + text(name, size, 3, 1))


def click(name, on):
    """L3 and R3: the stick pressed in, its name alone."""
    return svg(disc(on) + text(name, 60, 3, 2))


def dot(white, on):
    """White and Black: the pad's small pair, a disc of each colour."""
    fill, edge = ("#f4f2ec", "#000") if white else ("#08090c", "#fff")
    return svg(disc(on, R * 0.86) +
               f"<circle cx='{C}' cy='{C}' r='{R * 0.30}' fill='{fill}' stroke='{edge}' stroke-opacity='0.55' "
               f"stroke-width='5'/>")


def arrow(dx, dy, on):
    """A d-pad arrow: a round key with a solid triangle pointing its way."""
    t = R * 0.36
    tip = (C + dx * t, C + dy * t)
    b1 = (C - dx * t * 0.55 - dy * t * 0.85, C - dy * t * 0.55 + dx * t * 0.85)
    b2 = (C - dx * t * 0.55 + dy * t * 0.85, C - dy * t * 0.55 - dx * t * 0.85)
    tri = (f"<polygon points='{tip[0]:.2f},{tip[1]:.2f} {b1[0]:.2f},{b1[1]:.2f} {b2[0]:.2f},{b2[1]:.2f}' "
           f"fill='{GLYPH}' stroke='{GLYPH}' stroke-width='8' stroke-linejoin='round'/>")
    return svg(disc(on) + tri)


def stick_base():
    """The stick's well: a wide see-through disc, an inner ring where the thumb travels, and four small notches."""
    out = disc(False)
    out += (f"<circle cx='{C}' cy='{C}' r='{R * 0.62}' fill='none' stroke='#fff' stroke-opacity='0.14' "
            f"stroke-width='3'/>")
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        x, y = C + dx * R * 0.82, C + dy * R * 0.82
        out += f"<circle cx='{x:.1f}' cy='{y:.1f}' r='4.5' fill='#fff' fill-opacity='0.35'/>"
    return svg(out)


def knob(on):
    """The thumb: the same soft raised disc, fuller in the middle."""
    r = R * 0.94
    return svg(f"<circle cx='{C}' cy='{C}' r='{r}' fill='#fff' fill-opacity='{0.18 if on else 0.10}'/>"
               f"<clipPath id='in'><circle cx='{C}' cy='{C}' r='{r}'/></clipPath>"
               f"<g clip-path='url(#in)'><circle cx='{C}' cy='{C}' r='{r - 3}' fill='none' stroke='#fff' "
               f"stroke-opacity='{0.35 if on else 0.2}' stroke-width='10' filter='url(#soft)'/></g>"
               f"<circle cx='{C}' cy='{C}' r='{r - 1.5}' fill='none' stroke='#fff' "
               f"stroke-opacity='{0.65 if on else 0.45}' stroke-width='2.5'/>")


def gear(on):
    """Settings: a filled cog with a hole, on a disc."""
    pts = []
    step = 2 * math.pi / 8
    outer, inner = R * 0.56, R * 0.42
    for i in range(8):
        for k, rr in ((-0.28, inner), (-0.15, outer), (0.15, outer), (0.28, inner)):
            a = i * step + k * step
            pts.append(f"{C + math.cos(a) * rr:.2f},{C + math.sin(a) * rr:.2f}")
    h = R * 0.18
    return svg(disc(on) +
               f"<path fill-rule='evenodd' fill='{GLYPH}' d='M{' L'.join(pts)} Z "
               f"M{C + h},{C} A{h},{h} 0 1 0 {C - h},{C} A{h},{h} 0 1 0 {C + h},{C} Z' stroke='{GLYPH}' "
               f"stroke-width='3' stroke-linejoin='round'/>")


# the Xbox pad's letters
FACE = {"a": "A", "b": "B", "x": "X", "y": "Y"}


def art():
    out = {}
    for on in (False, True):
        s = "_on" if on else ""
        for key, ch in FACE.items():
            out[f"ctl_{key}{s}"] = letter(ch, on)
        out[f"ctl_lt{s}"] = label("LT", 60, on)
        out[f"ctl_rt{s}"] = label("RT", 60, on)
        out[f"ctl_start{s}"] = label("START", 36, on)
        out[f"ctl_back{s}"] = label("BACK", 40, on)
        out[f"ctl_l3{s}"] = click("L3", on)
        out[f"ctl_r3{s}"] = click("R3", on)
        out[f"ctl_white{s}"] = dot(True, on)
        out[f"ctl_black{s}"] = dot(False, on)
        out[f"ctl_gear{s}"] = gear(on)
        for n, d in (("up", (0, -1)), ("down", (0, 1)), ("left", (-1, 0)), ("right", (1, 0))):
            out[f"ctl_arrow_{n}{s}"] = arrow(*d, on)
        out[f"ctl_knob{s}"] = knob(on)
    out["ctl_stick"] = stick_base()
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in art().items():
            src = Path(tmp) / f"{name}.svg"
            src.write_text(body)
            subprocess.run(["rsvg-convert", str(src), "-w", str(PX), "-h", str(PX), "-o", str(OUT / f"{name}.png")],
                           check=True)
    print(f"{OUT}: {len(art())} images")


if __name__ == "__main__":
    main()
