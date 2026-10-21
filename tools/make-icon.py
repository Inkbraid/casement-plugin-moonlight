"""Draws icon.png: a monitor showing a crescent moon and a play sign, on a
transparent background. Own artwork; run from the repository root."""
from PIL import Image, ImageDraw

S = 4  # drawn at 4x and scaled down for smooth edges
img = Image.new("RGBA", (256 * S, 256 * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
frame, screen = (30, 36, 58, 255), (44, 66, 128, 255)
moon, white = (247, 228, 160, 255), (255, 255, 255, 255)


def box(x0, y0, x1, y1):
    return [v * S for v in (x0, y0, x1, y1)]


def poly(points):
    return [(x * S, y * S) for x, y in points]


d.rounded_rectangle(box(16, 32, 240, 186), radius=18 * S, fill=frame)  # monitor
d.rounded_rectangle(box(30, 46, 226, 172), radius=8 * S, fill=screen)  # screen
d.polygon(poly([(104, 186), (152, 186), (162, 218), (94, 218)]), fill=frame)
d.rounded_rectangle(box(72, 214, 184, 230), radius=6 * S, fill=frame)  # stand
# A crescent: a disc with an offset disc of the screen's colour over it.
d.ellipse(box(54, 62, 146, 154), fill=moon)
d.ellipse(box(80, 50, 172, 142), fill=screen)
# The stream: a play sign.
d.polygon(poly([(150, 92), (202, 122), (150, 152)]), fill=white)

img.resize((256, 256), Image.LANCZOS).save("icon.png")
