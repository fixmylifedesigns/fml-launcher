"""Make cover files for CubeLaunch.

Covers live on the SD card as covers/<KEY>.cov, where KEY is a disc's game ID
(e.g. GALE01), or the file name without extension for a .dol or a Game Boy ROM.
Each .cov holds front, spine and back textures already in the GameCube's
RGB565 tile format, so the console just loads them onto the GPU.

  python tools/covers.py fetch E:\\                 download covers for every game on the card
  python tools/covers.py add E:\\ GALE01 wrap.jpg   make a cover from your own image
  python tools/covers.py add E:\\ vbagx front.png back.png "VBA GX"
                                                    separate front and back (spine made to match)
  python tools/covers.py embed source/demo_covers.c covers/*.cov
                                                    bake covers into the demo build (for Dolphin)

Images wider than tall are treated as a full wrap (back | spine | front), like
GameTDB's "coverfull" scans; anything else is used as the front only.
GameCube downloads come from GameTDB (art.gametdb.com); Game Boy, Game Boy Color
and Game Boy Advance box art from libretro-thumbnails, matched by No-Intro file name.
"""
import io
import re
import os
import struct
import sys
import urllib.parse
import urllib.request

from PIL import Image, ImageDraw, ImageFont, ImageOps

FRONT = (160, 228)
FRONT_GB_BOX = (192, 220)  # Game Boy boxes are wider and shorter
SPINE_GB_BOX = (28, 220)
SPINE = (24, 228)
BACK = (160, 228)

# Share of a full wrap's width taken by each part (measured on GameCube scans)
WRAP_BACK = 0.461
WRAP_SPINE = 0.060

REGIONS = {
    "E": ["US"], "J": ["JA"], "K": ["KO"], "D": ["DE"], "F": ["FR"], "S": ["ES"], "I": ["IT"],
    "P": ["EN", "FR", "DE", "ES", "IT", "NL"],
}
KINDS = ["coverfullHQ", "coverfull", "cover"]


def to_rgb565_tiles(img, size):
    """Resize to size and encode as GameCube RGB565 4x4 tiles (big endian)."""
    w, h = size
    px = img.convert("RGB").resize(size, Image.LANCZOS).load()
    out = bytearray()
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    r, g, b = px[x, y]
                    out += struct.pack(">H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


def make_cov(img, front_size=FRONT, spine=None, back=None, spine_size=SPINE, back_size=BACK):
    """Build .cov bytes from a wrap or front-only image, plus an optional spine and back."""
    img = img.convert("RGB")
    w, h = img.size
    parts = {}
    if spine is not None:
        parts["spine"] = spine
    if back is not None:
        parts["back"] = back
    # A wide image is a full wrap (back | spine | front) unless the back and spine came
    # separately: then it's a landscape front, like a Japanese Game Boy Advance box.
    if spine is None and back is None and w > h * 1.1:
        back_w, spine_w = round(w * WRAP_BACK), round(w * WRAP_SPINE)
        parts["back"] = img.crop((0, 0, back_w, h))
        parts["spine"] = img.crop((back_w, 0, back_w + spine_w, h))
        parts["front"] = img.crop((back_w + spine_w, 0, w, h))
    else:
        parts["front"] = img
    out = bytearray(b"CCV1")
    for name, size in (("front", front_size), ("spine", spine_size), ("back", back_size)):
        if name in parts:
            out += struct.pack(">HH", *size) + to_rgb565_tiles(parts[name], size)
        else:
            out += struct.pack(">HH", 0, 0)
    return bytes(out)


def read_game_id(path):
    with open(path, "rb") as f:
        head = f.read(0x20)
    if len(head) < 0x20 or struct.unpack(">I", head[0x1C:0x20])[0] != 0xC2339F3D:
        return None
    gid = head[:6].decode("ascii", "replace")
    return gid if gid.isalnum() else None


def download(gid):
    for kind in KINDS:
        for region in REGIONS.get(gid[3], ["US", "EN", "JA"]):
            url = f"https://art.gametdb.com/wii/{kind}/{region}/{gid}.png"
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "CubeLaunch cover tool"})
                with urllib.request.urlopen(req, timeout=20) as r:
                    return Image.open(io.BytesIO(r.read())), url
            except Exception:
                continue
    return None, None


LIBRETRO = {
    ".gb": "Nintendo_-_Game_Boy",
    ".gbc": "Nintendo_-_Game_Boy_Color",
    ".gba": "Nintendo_-_Game_Boy_Advance",
}


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "FML Launcher cover tool"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def rom_name_variants(base):
    """The exact No-Intro name first, then looser ones for older libretro names."""
    names = [base]
    no_rev = re.sub(r" \((Rev [^)]*|v[0-9.]+)\)", "", base)
    first_group = re.sub(r"^(.*?\([^)]*\)).*$", r"\1", base)
    for n in (no_rev, first_group):
        if n not in names:
            names.append(n)
    return names


def download_rom_art(ext, base, folder_name="Named_Boxarts"):
    """Box art from libretro-thumbnails, which names files after No-Intro ROM names."""
    for variant in rom_name_variants(base):
        name = "".join("_" if c in '&*/:`<>?\\|"' else c for c in variant)
        folder = f"https://raw.githubusercontent.com/libretro-thumbnails/{LIBRETRO[ext]}/master/{folder_name}/"
        url = folder + urllib.parse.quote(name) + ".png"
        try:
            data = fetch(url)
            # Some entries are symlinks, which download as the target's file name
            if not data.startswith(b"\x89PNG") and len(data) < 512:
                url = folder + urllib.parse.quote(data.decode("utf-8").strip())
                data = fetch(url)
            img = Image.open(io.BytesIO(data)).convert("RGBA")
        except Exception:
            continue
        # Trim the transparent padding around the box
        box = img.getchannel("A").getbbox()
        if box:
            img = img.crop(box)
        return img.convert("RGB"), url
    return None, None


PLATFORM = {".gb": ("GAME BOY", "GB"), ".gbc": ("GAME BOY COLOR", "GBC"), ".gba": ("GAME BOY ADVANCE", "GBA")}
FONT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts", "Inter.ttf")


def font(size, weight):
    f = ImageFont.truetype(FONT_PATH, size)
    f.set_variation_by_axes([max(14, min(32, size)), weight])
    return f


def box_colour(front):
    """The front's main colour, darkened, for the back and spine."""
    small = front.convert("RGB").resize((24, 24))
    pixels = small.get_flattened_data() if hasattr(small, "get_flattened_data") else small.getdata()
    px = sorted(pixels, key=lambda c: sum(c))
    mid = px[len(px) // 3: 2 * len(px) // 3]  # skip the darkest and brightest
    r, g, b = (sum(c[i] for c in mid) // len(mid) for i in range(3))
    return (int(r * 0.45), int(g * 0.45), int(b * 0.45))


def wrap(draw, text, fnt, width):
    lines, line = [], ""
    for word in text.split():
        test = (line + " " + word).strip()
        if draw.textlength(test, font=fnt) <= width or not line:
            line = test
        else:
            lines.append(line)
            line = word
    if line:
        lines.append(line)
    return lines


def rom_title(base):
    return re.sub(r" [\(\[].*$", "", base)


def rom_spine(front, base, ext):
    """Spine for a Game Boy box: platform badge at the top, title reading top to bottom."""
    return make_spine(front, rom_title(base), PLATFORM[ext][1], SPINE_GB_BOX)


def make_spine(front, title, short, size):
    """Spine in the front's colour, with a badge at the top and the title down it."""
    W, H = size[0] * 3, size[1] * 3
    colour = box_colour(front)
    img = Image.new("RGB", (W, H), colour)
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W, 64), fill=(245, 245, 245))
    f = font(30 if len(short) < 3 else 24, 900)
    d.text((W / 2, 33), short, font=f, fill=(20, 20, 30), anchor="mm")
    strip = Image.new("RGB", (H - 90, W), colour)
    sd = ImageDraw.Draw(strip)
    f = font(34, 800)
    title = title.upper()
    while sd.textlength(title, font=f) > strip.width - 20 and len(title) > 4:
        title = title[:-2]
    sd.text((10, W / 2), title, font=f, fill="white", anchor="lm")
    img.paste(strip.rotate(-90, expand=True), (0, 80))
    return img.resize(size, Image.LANCZOS)


def rom_back(front, base, ext, title_img, snap_img):
    """Back: platform band, title, title-screen and gameplay shots."""
    W, H = FRONT_GB_BOX[0] * 3, FRONT_GB_BOX[1] * 3
    top = box_colour(front)
    bottom = tuple(int(c * 0.45) for c in top)
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        d.line((0, y, W, y), fill=tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)))

    # Platform band
    d.rectangle((0, 0, W, 58), fill=(245, 245, 245))
    d.text((24, 29), PLATFORM[ext][0], font=font(30, 900), fill=(20, 20, 30), anchor="lm")

    # Title
    f = font(40, 800)
    y = 86
    for line in wrap(d, rom_title(base), f, W - 48)[:2]:
        d.text((24, y), line, font=f, fill="white")
        y += 48

    # Screenshots: title screen large on the left, gameplay overlapping bottom right
    shots = [im for im in (title_img, snap_img) if im is not None]
    area_top, area_bottom = y + 18, H - 90

    def framed(im, width):
        h = round(width * im.height / im.width)
        shot = ImageOps.expand(im.convert("RGB").resize((width, h), Image.LANCZOS), border=6, fill="white")
        shadow = Image.new("RGBA", (shot.width + 12, shot.height + 12), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rectangle((8, 8, shadow.width - 1, shadow.height - 1), fill=(0, 0, 0, 110))
        shadow.paste(shot, (0, 0))
        return shadow

    if len(shots) == 2:
        a = framed(shots[0], 340)
        b = framed(shots[1], 290)
        img.paste(a, (24, area_top), a)
        img.paste(b, (W - 24 - b.width, min(area_bottom - b.height, area_top + a.height - 40)), b)
    elif shots:
        a = framed(shots[0], W - 120)
        img.paste(a, ((W - a.width) // 2, area_top), a)

    # Bottom band
    d.rectangle((0, H - 70, W, H), fill=tuple(int(c * 0.6) for c in bottom))
    region = rom_region(base)
    d.text((24, H - 35), "1 PLAYER" + (f"  \u00b7  {region.upper()}" if region else ""), font=font(24, 700),
           fill=(220, 220, 230), anchor="lm")
    d.text((W - 24, H - 35), "FML LAUNCHER", font=font(22, 800), fill=(150, 150, 170), anchor="rm")
    return img.resize(FRONT_GB_BOX, Image.LANCZOS)


def box_size(img):
    """Texture size keeping the box art's proportions (Japanese boxes differ from US ones).
    libretro stretches many US/European scans to an exact square, so those keep the standard shape."""
    if img.width == img.height:
        return FRONT_GB_BOX
    w = FRONT_GB_BOX[0]
    h = round(w * img.height / img.width / 4) * 4
    return (w, max(96, min(252, h)))  # landscape Japanese GBA boxes are wider than tall


def rom_cov(img, ext, base):
    """Front from the box art, plus a generated spine and back, all at the box's real shape."""
    size = box_size(img)
    title_img, _ = download_rom_art(ext, base, "Named_Titles")
    snap_img, _ = download_rom_art(ext, base, "Named_Snaps")
    spine = make_spine(img, rom_title(base), PLATFORM[ext][1], (SPINE_GB_BOX[0], size[1]))
    back = rom_back(img, base, ext, title_img, snap_img).resize(size, Image.LANCZOS)
    return make_cov(img, size, spine, back, (SPINE_GB_BOX[0], size[1]), size)


def rom_region(base):
    m = re.search(r"\(([^)]*(Japan|USA|Europe|World|Korea|Germany|France|Spain|Italy|Australia)[^)]*)\)", base)
    return m.group(1) if m else ""


def save(sd_root, key, data):
    os.makedirs(os.path.join(sd_root, "covers"), exist_ok=True)
    path = os.path.join(sd_root, "covers", key + ".cov")
    with open(path, "wb") as f:
        f.write(data)
    return path


def cmd_fetch(sd_root):
    for dirpath, dirs, files in os.walk(sd_root):
        dirs[:] = [d for d in dirs if not d.startswith(".") and d.lower() != "covers"]
        for name in files:
            base, ext = os.path.splitext(name)
            ext = ext.lower()
            full = os.path.join(dirpath, name)
            if ext in (".iso", ".gcm"):
                gid = read_game_id(full)
                if not gid:
                    print(f"skip   {name}: not a GameCube disc image")
                    continue
                if os.path.exists(os.path.join(sd_root, "covers", gid + ".cov")):
                    print(f"have   {gid}  {name}")
                    continue
                img, url = download(gid)
                if img is None:
                    print(f"miss   {gid}  {name}: no cover on GameTDB")
                    continue
                save(sd_root, gid, make_cov(img))
                print(f"saved  {gid}  {name}  <- {url}")
            elif ext in LIBRETRO:
                if os.path.exists(os.path.join(sd_root, "covers", base + ".cov")):
                    print(f"have   {name}")
                    continue
                img, url = download_rom_art(ext, base)
                if img is None:
                    print(f"miss   {name}: no box art on libretro-thumbnails")
                    continue
                save(sd_root, base, rom_cov(img, ext, base))
                print(f"saved  {name}  <- {url}")
            elif ext == ".dol":
                for img_ext in (".png", ".jpg", ".jpeg"):
                    src = os.path.join(dirpath, base + img_ext)
                    if os.path.exists(src):
                        save(sd_root, base, make_cov(Image.open(src)))
                        print(f"saved  {base}  from {os.path.basename(src)}")
                        break


def cmd_add(sd_root, key, image, back=None, title=None):
    """A wrap or front image; with a separate back, a matching spine is made too."""
    front = Image.open(image).convert("RGB")
    if back:
        spine = make_spine(front, title or key, "APP", SPINE)
        data = make_cov(front, FRONT, spine, Image.open(back).convert("RGB"), SPINE, BACK)
    else:
        data = make_cov(front)
    path = save(sd_root, key, data)
    print(f"saved  {path}")


def cmd_embed(out_c, covs):
    lines = ["// Generated by tools/covers.py embed. Covers baked into the demo build.",
             '#include "demo_covers.h"', "",
             "// Only the demo build (make DEMO=1) carries them, to keep the Pico payload small",
             "#ifdef START_DEMO"]
    table = []
    for i, path in enumerate(covs):
        key = os.path.splitext(os.path.basename(path))[0]
        data = open(path, "rb").read()
        lines.append(f"static const u8 cover_{i}[] __attribute__((aligned(32))) = {{")
        for j in range(0, len(data), 32):
            lines.append("\t" + ",".join(str(b) for b in data[j:j + 32]) + ",")
        lines.append("};")
        table.append(f'\t{{ "{key}", cover_{i}, sizeof(cover_{i}) }},')
    lines.append("const DemoCover demo_covers[] = {")
    lines += table or ["\t{ 0, 0, 0 },"]
    lines.append("};")
    lines.append(f"const int demo_cover_count = {len(covs)};")
    lines += ["#else", "const DemoCover demo_covers[] = { { 0, 0, 0 } };", "const int demo_cover_count = 0;", "#endif"]
    with open(out_c, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"embedded {len(covs)} cover(s) into {out_c}")


def main(argv):
    if len(argv) >= 2 and argv[0] == "fetch":
        cmd_fetch(argv[1])
    elif len(argv) in (4, 5, 6) and argv[0] == "add":
        cmd_add(*argv[1:])
    elif len(argv) >= 2 and argv[0] == "embed":
        cmd_embed(argv[1], argv[2:])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
