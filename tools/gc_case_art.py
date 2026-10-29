"""Turn plain box art into a GameCube-style case insert.

  python tools/gc_case_art.py front.png back.png out_dir "Title on spine" [publisher]

Adds the black banner GameCube inserts have across the top (cube emblem and a
GAMECUBE wordmark, publisher on the left) above the front and back art, and
makes a matching black spine. Writes cover_front.png, cover_back.png and
cover_spine.png into out_dir.
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from covers import font  # noqa: E402

PURPLE_TOP = (124, 104, 220)
PURPLE_LEFT = (82, 64, 168)
PURPLE_RIGHT = (54, 40, 122)


def cube(draw, cx, cy, r):
    """Isometric cube emblem made of three shaded faces."""
    h = r * 0.5
    top = [(cx, cy - r), (cx + r * 0.87, cy - h), (cx, cy), (cx - r * 0.87, cy - h)]
    left = [(cx - r * 0.87, cy - h), (cx, cy), (cx, cy + r), (cx - r * 0.87, cy + h)]
    right = [(cx + r * 0.87, cy - h), (cx + r * 0.87, cy + h), (cx, cy + r), (cx, cy)]
    draw.polygon(top, fill=PURPLE_TOP)
    draw.polygon(left, fill=PURPLE_LEFT)
    draw.polygon(right, fill=PURPLE_RIGHT)
    # Small inner cube cut into the top-left, like a letter G
    s = r * 0.42
    inner = [(cx - s * 0.87, cy - h * 0.15), (cx, cy + s * 0.35), (cx, cy + s * 1.1), (cx - s * 0.87, cy + h * 0.6)]
    draw.polygon(inner, fill=(20, 16, 40))


def banner(width, height, publisher, scale=1.0):
    img = Image.new("RGB", (width, height), (8, 8, 10))
    d = ImageDraw.Draw(img)
    pad = int(height * 0.28)
    r = height * 0.32 * scale
    word = font(int(height * 0.36 * scale), 900)
    text = "GAMECUBE"
    spacing = height * 0.035 * scale
    text_w = sum(d.textlength(c, font=word) for c in text) + spacing * (len(text) - 1)
    x = width - pad - text_w
    cy = height / 2
    cube(d, x - r * 1.3, cy, r)
    for c in text:
        d.text((x, cy), c, font=word, fill=(245, 245, 245), anchor="lm")
        x += d.textlength(c, font=word) + spacing
    if publisher:
        d.text((pad, cy), publisher, font=font(int(height * 0.26 * scale), 800), fill=(235, 235, 240), anchor="lm")
    # Thin silver rule under the banner
    d.rectangle((0, height - 6, width, height), fill=(150, 150, 165))
    return img


def with_banner(art, publisher, scale=1.0):
    art = art.convert("RGB")
    band_h = round(art.width * 0.15)
    out = Image.new("RGB", (art.width, art.height + band_h))
    out.paste(banner(art.width, band_h, publisher, scale), (0, 0))
    out.paste(art, (0, band_h))
    return out


def spine(height, title, width_ratio=0.08):
    w = max(64, round(height * width_ratio))
    img = Image.new("RGB", (w, height), (8, 8, 10))
    d = ImageDraw.Draw(img)
    cube(d, w / 2, w * 0.75, w * 0.32)
    strip = Image.new("RGB", (height - int(w * 3.2), w), (8, 8, 10))
    sd = ImageDraw.Draw(strip)
    sd.text((w * 0.2, w / 2), title.upper(), font=font(int(w * 0.42), 900), fill="white", anchor="lm")
    sd.text((strip.width - w * 0.2, w / 2), "GAMECUBE", font=font(int(w * 0.26), 800), fill=(190, 190, 200), anchor="rm")
    img.paste(strip.rotate(-90, expand=True), (0, int(w * 1.4)))
    return img


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 1
    front_path, back_path, out_dir, title = argv[:4]
    publisher = argv[4] if len(argv) > 4 else ""
    os.makedirs(out_dir, exist_ok=True)
    front = with_banner(Image.open(front_path), publisher)
    back = with_banner(Image.open(back_path), publisher, scale=0.8)
    front.save(os.path.join(out_dir, "cover_front.png"))
    back.save(os.path.join(out_dir, "cover_back.png"))
    spine(front.height, title).save(os.path.join(out_dir, "cover_spine.png"))
    print(f"wrote cover_front.png {front.size}, cover_back.png {back.size}, cover_spine.png in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
