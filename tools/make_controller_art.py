#!/usr/bin/env python3
"""Draw the controller pictures for the game's controls pages (0.3.0).

    python3 tools/make_controller_art.py [--preview DIR]

The game's controls page shows hud/MogaController.tif, a drawing of the MOGA
gamepad it was made for, with callout lines that point at its buttons. The
port replaces that drawing (src/game/controller_art.cpp) with one of these two
original drawings, made here from code in the game's colours:

    controller   a gamepad (shown when the console is docked)
    handheld     a handheld console (shown in handheld mode)

Neither copies any real product: they show the Switch's button names in the
Switch's arrangement (X top, Y left, A right, B bottom), with every element
where one of the game's callout lines points (the lines are part of the game's
layout, not of the picture).

The picture is 628x431 pixels, but the game draws it squashed to 0.866 of its
height on a 16:9 screen; everything is drawn in that squashed space (so circles
come out round on the console) and stretched back at the end.

Writes src/game/controller_art_images.inc: both pictures as PNG files (straight
alpha) in C arrays. Needs Pillow. Letters are drawn with Poppins Bold (SIL Open
Font License) when it is installed, else with a built-in fallback.
"""
import argparse
import io
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "src", "game", "controller_art_images.inc")

W, H = 628, 431          # the picture in the texture atlases
SQUASH = 0.8663          # height / width scale the game draws it at on 16:9
DH = H * SQUASH          # design-space height (373.4)
SS = 4                   # supersampling

# The game's colours (from its own UI).
BODY = (64, 81, 89)
BODY_LIGHT = (75, 95, 104)
BODY_EDGE = (36, 45, 50)
WELL = (43, 54, 60)
PANEL = (53, 67, 74)
CAP = (200, 203, 205)
CAP_RIM = (232, 234, 235)
WHITE = (255, 255, 255)
SCREEN = (20, 24, 28)

# Where the game's callout lines point, in design space (x, squashed y). The
# elements are placed so that every line ends on the right one (the drift line
# of the game controls page is moved to A by the port, see obb_patch.cpp).
L_TAB = (150, 6, 216, 56)
R_TAB = (412, 6, 478, 56)
LEFT_STICK = (146, 143)
RIGHT_STICK = (448, 236)
FACE = {"X": (475, 111.5), "Y": (434, 145.5), "A": (516, 145.5), "B": (475, 179.5)}
MINUS = (246, 66)
PLUS = (382, 66)
DPAD = (150, 248)


def font(size):
    for path in ("/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, int(size * SS))
    return ImageFont.load_default()


class Canvas:
    """Drawing in design space, supersampled."""

    def __init__(self):
        self.img = Image.new("RGBA", (W * SS, int(round(DH * SS))), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)

    @staticmethod
    def s(v):
        return v * SS

    def rrect(self, box, r, fill, outline=None, width=0):
        x0, y0, x1, y1 = box
        self.d.rounded_rectangle([self.s(x0), self.s(y0), self.s(x1), self.s(y1)], radius=self.s(r), fill=fill,
                                 outline=outline, width=int(self.s(width)))

    def circle(self, c, r, fill=None, outline=None, width=0):
        x, y = c
        self.d.ellipse([self.s(x - r), self.s(y - r), self.s(x + r), self.s(y + r)], fill=fill, outline=outline,
                       width=int(self.s(width)))

    def ellipse(self, box, fill):
        x0, y0, x1, y1 = box
        self.d.ellipse([self.s(x0), self.s(y0), self.s(x1), self.s(y1)], fill=fill)

    def text(self, c, s, size, fill=WHITE):
        f = font(size)
        self.d.text((self.s(c[0]), self.s(c[1])), s, font=f, fill=fill, anchor="mm")

    def bar(self, c, w, h, fill=WHITE):
        x, y = c
        self.rrect((x - w / 2, y - h / 2, x + w / 2, y + h / 2), min(w, h) / 2, fill)


def body_mask(shape_fn):
    m = Image.new("L", (W * SS, int(round(DH * SS))), 0)
    shape_fn(ImageDraw.Draw(m), 255)
    return m


def shadowed(c, mask):
    """A soft dark shadow around the body, under everything (as the game's
    own picture has)."""
    sh = mask.filter(ImageFilter.GaussianBlur(7 * SS)).point(lambda v: int(v * 0.55))
    black = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    black.putalpha(sh)
    base = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    base.alpha_composite(black, (0, int(3 * SS)))
    return base


def finish(c, base):
    base.alpha_composite(c.img)
    out = base.resize((W, H), Image.LANCZOS)
    return out


# ---- the parts every picture shares -------------------------------------------
def shoulders(c):
    for box, label in ((L_TAB, "L"), (R_TAB, "R")):
        c.rrect(box, 14, PANEL, BODY_EDGE, 2)
        # the letter in the part above the body
        c.text(((box[0] + box[2]) / 2, box[1] + 18), label, 19)


def stick(c, center):
    c.circle(center, 39, WELL)
    c.circle(center, 29, CAP_RIM)
    c.circle(center, 25.5, CAP)
    c.circle(center, 18, None, (186, 189, 191), 1.5)


def face_buttons(c):
    cx, cy = FACE["X"][0], (FACE["Y"][1])
    c.ellipse((cx - 61, cy - 55, cx + 61, cy + 55), WELL)
    for label, pos in FACE.items():
        c.circle(pos, 17, BODY_EDGE)
        c.circle(pos, 15.5, WHITE)
        c.circle(pos, 12.8, WELL)
        c.text((pos[0], pos[1] + 1), label, 17)


def system_buttons(c):
    for pos, plus in ((MINUS, False), (PLUS, True)):
        c.circle(pos, 12.5, BODY_EDGE)
        c.circle(pos, 11, CAP)
        c.bar(pos, 11, 3, WELL)
        if plus:
            c.bar(pos, 3, 11, WELL)


def dpad(c):
    x, y = DPAD
    c.circle(DPAD, 31, WELL)
    arm, half = 23, 8.5
    for box in ((x - half, y - arm, x + half, y + arm), (x - arm, y - half, x + arm, y + half)):
        c.rrect((box[0] - 1.5, box[1] - 1.5, box[2] + 1.5, box[3] + 1.5), 4, BODY_EDGE)
    for box in ((x - half, y - arm, x + half, y + arm), (x - arm, y - half, x + arm, y + half)):
        c.rrect(box, 3, CAP)
    c.circle(DPAD, 4, (186, 189, 191))
    for pts in ([(0, -18), (-3.5, -13), (3.5, -13)], [(0, 18), (-3.5, 13), (3.5, 13)],
                [(-18, 0), (-13, -3.5), (-13, 3.5)], [(18, 0), (13, -3.5), (13, 3.5)]):
        c.d.polygon([(c.s(x + px), c.s(y + py)) for px, py in pts], fill=WELL)


# ---- the gamepad --------------------------------------------------------------
def controller_shape(d, fill, inset=0):
    s = lambda v: v * SS
    i = inset
    # a wide rounded body and two grips
    d.rounded_rectangle([s(26 + i), s(40 + i), s(602 - i), s(262 - i)], radius=s(78 - i), fill=fill)
    d.ellipse([s(28 + i), s(150 + i), s(232 - i), s(352 - i)], fill=fill)
    d.ellipse([s(396 + i), s(150 + i), s(600 - i), s(352 - i)], fill=fill)
    d.rounded_rectangle([s(150 + i), s(150 + i), s(478 - i), s(292 - i)], radius=s(60 - i), fill=fill)


def controller():
    c = Canvas()
    shoulders(c)
    controller_shape(c.d, BODY_EDGE)
    controller_shape(c.d, BODY, inset=2.5)
    # a lighter top face
    hl = Image.new("L", c.img.size, 0)
    controller_shape(ImageDraw.Draw(hl), 255, inset=10)
    hl = hl.filter(ImageFilter.GaussianBlur(10 * SS)).point(lambda v: int(v * 0.35))
    light = Image.new("RGBA", c.img.size, BODY_LIGHT + (0,))
    light.putalpha(hl)
    c.img.alpha_composite(light)
    stick(c, LEFT_STICK)
    stick(c, RIGHT_STICK)
    face_buttons(c)
    dpad(c)
    system_buttons(c)
    mask = body_mask(lambda d, f: controller_shape(d, f))
    return finish(c, shadowed(c, mask))


# ---- the handheld -------------------------------------------------------------
SCREEN_BOX = (226, 96, 400, 222)


def handheld_shape(d, fill, inset=0):
    s = lambda v: v * SS
    i = inset
    d.rounded_rectangle([s(18 + i), s(44 + i), s(610 - i), s(330 - i)], radius=s(56 - i), fill=fill)


def screen(c):
    x0, y0, x1, y1 = SCREEN_BOX
    c.rrect((x0 - 8, y0 - 8, x1 + 8, y1 + 8), 12, (28, 34, 38))
    c.rrect(SCREEN_BOX, 4, SCREEN)
    # a simple road towards the horizon, in the game's red and greys
    sx = lambda v: c.s(v)
    hz = y0 + (y1 - y0) * 0.42
    sky = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sky)
    for k in range(int(sx(hz - y0))):
        t = k / max(1, sx(hz - y0))
        col = (int(36 + 30 * t), int(44 + 26 * t), int(54 + 20 * t), 255)
        sd.line([(sx(x0), sx(y0) + k), (sx(x1), sx(y0) + k)], fill=col)
    road = [(sx(x0), sx(y1)), (sx(x1), sx(y1)), (sx((x0 + x1) / 2 + 6), sx(hz)), (sx((x0 + x1) / 2 - 6), sx(hz))]
    sd.polygon([(sx(x0), sx(hz)), (sx(x1), sx(hz)), (sx(x1), sx(y1)), (sx(x0), sx(y1))], fill=(30, 36, 40, 255))
    sd.polygon(road, fill=(58, 64, 68, 255))
    mid = (x0 + x1) / 2
    for k in range(5):
        t0, t1 = (k + 0.15) / 5, (k + 0.6) / 5
        ya, yb = hz + (y1 - hz) * t0 ** 1.6, hz + (y1 - hz) * t1 ** 1.6
        wa, wb = 0.6 + 2.2 * t0 ** 1.6, 0.6 + 2.2 * t1 ** 1.6
        sd.polygon([(sx(mid - wa), sx(ya)), (sx(mid + wa), sx(ya)), (sx(mid + wb), sx(yb)), (sx(mid - wb), sx(yb))],
                   fill=(210, 212, 214, 255))
    sd.rectangle([sx(x0), sx(hz) - 1, sx(x1), sx(hz) + 1], fill=(142, 22, 26, 255))
    mask = Image.new("L", c.img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([sx(x0), sx(y0), sx(x1), sx(y1)], radius=sx(4), fill=255)
    clipped = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    clipped.paste(sky, (0, 0), mask)
    c.img.alpha_composite(clipped)
    # a faint reflection
    refl = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    rd = ImageDraw.Draw(refl)
    rd.polygon([(sx(x0 + 20), sx(y0)), (sx(x0 + 70), sx(y0)), (sx(x0 + 20), sx(y1)), (sx(x0 - 30), sx(y1))],
               fill=(255, 255, 255, 14))
    clipped = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    clipped.paste(refl, (0, 0), mask)
    c.img.alpha_composite(clipped)


def handheld():
    c = Canvas()
    shoulders(c)
    handheld_shape(c.d, BODY_EDGE)
    handheld_shape(c.d, BODY, inset=2.5)
    hl = Image.new("L", c.img.size, 0)
    handheld_shape(ImageDraw.Draw(hl), 255, inset=10)
    hl = hl.filter(ImageFilter.GaussianBlur(10 * SS)).point(lambda v: int(v * 0.35))
    light = Image.new("RGBA", c.img.size, BODY_LIGHT + (0,))
    light.putalpha(hl)
    c.img.alpha_composite(light)
    screen(c)
    stick(c, LEFT_STICK)
    stick(c, RIGHT_STICK)
    face_buttons(c)
    dpad(c)
    system_buttons(c)
    mask = body_mask(lambda d, f: handheld_shape(d, f))
    return finish(c, shadowed(c, mask))


def png_bytes(img):
    b = io.BytesIO()
    img.save(b, "PNG", optimize=True)
    return b.getvalue()


def c_array(name, data):
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 20):
        lines.append("    " + ",".join(str(v) for v in data[i:i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", help="also save the two pictures as PNG files in this folder")
    a = ap.parse_args()
    pics = {"controller": controller(), "handheld": handheld()}
    if a.preview:
        os.makedirs(a.preview, exist_ok=True)
        for k, im in pics.items():
            im.save(os.path.join(a.preview, f"{k}.png"))
    parts = ["// Generated by tools/make_controller_art.py: the two pictures for the",
             "// controls pages (628x431, PNG, straight alpha). Do not edit.",
             "// clang-format off"]
    for k, im in pics.items():
        parts.append(c_array(f"k_{k}_png", png_bytes(im)))
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(parts) + "\n")
    print(OUT, os.path.getsize(OUT) // 1024, "KiB")


if __name__ == "__main__":
    main()
