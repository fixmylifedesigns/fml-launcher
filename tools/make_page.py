"""Save a web page for FML Launcher's page viewer.

  python tools/make_page.py https://fixmylifeco.com/mods/gamecube/picoloader D:/ "PicoLoader Guide"

Chrome (on this PC) renders the page once at the GameCube's 640 px width. The
picture is written to <card>/web/<title>.fpg as horizontal strips the console
can stream while you scroll, and a matching case cover goes to <card>/covers/.
Re-run it whenever the page changes.

.fpg layout (big endian): "FPG1", u16 width, u16 height, u16 strip height,
u16 strip count, 48-byte title, padding to 64 bytes, then the strips as
GameCube RGB565 4x4 tiles (the last strip padded).

  --max-height N   keep only the top N pixels (used for the Dolphin demo build)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import covers  # noqa: E402

WIDTH = 640
STRIP_H = 256
CHROME_PATHS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]


def capture(url, max_height=20000):
    """Full-height screenshot of the page at 640 px wide."""
    browser = next((p for p in CHROME_PATHS if os.path.exists(p)), None)
    if not browser:
        sys.exit("Chrome or Edge is needed to capture the page")
    tmp = tempfile.mkdtemp()
    shot = os.path.join(tmp, "page.png")
    subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                    f"--user-data-dir={os.path.join(tmp, 'profile')}", f"--window-size={WIDTH},{max_height}",
                    "--virtual-time-budget=15000", f"--screenshot={shot}", url],
                   check=True, capture_output=True)
    img = Image.open(shot).convert("RGB")
    shutil.rmtree(tmp, ignore_errors=True)

    # Trim the empty area below the page
    px = img.load()
    bg = px[WIDTH - 1, img.height - 1]
    y = img.height - 1
    while y > 0 and all(px[x, y] == bg for x in range(0, WIDTH, 8)):
        y -= 1
    return img.crop((0, 0, WIDTH, min(img.height, y + 24)))


def write_fpg(img, title, path):
    strips = (img.height + STRIP_H - 1) // STRIP_H
    header = b"FPG1" + struct.pack(">4H", WIDTH, img.height, STRIP_H, strips)
    header += title.encode("ascii", "replace")[:47].ljust(48, b"\0")
    header = header.ljust(64, b"\0")
    with open(path, "wb") as f:
        f.write(header)
        for i in range(strips):
            strip = Image.new("RGB", (WIDTH, STRIP_H), img.getpixel((0, img.height - 1)))
            strip.paste(img.crop((0, i * STRIP_H, WIDTH, min(img.height, (i + 1) * STRIP_H))), (0, 0))
            f.write(covers.to_rgb565_tiles(strip, (WIDTH, STRIP_H)))
    return strips


def page_cov(img, title):
    """Front: the top of the page. Back: a section further down. Spine: WEB badge."""
    case_h = round(WIDTH * covers.FRONT[1] / covers.FRONT[0])
    front = img.crop((0, 0, WIDTH, min(img.height, case_h)))
    back_top = min(max(0, img.height - case_h), case_h + 200)
    back = img.crop((0, back_top, WIDTH, min(img.height, back_top + case_h)))
    spine = covers.make_spine(front, title, "WEB", covers.SPINE)
    return covers.make_cov(front, covers.FRONT, spine, back, covers.SPINE, covers.BACK)


def main(argv):
    max_height = None
    if "--max-height" in argv:
        i = argv.index("--max-height")
        max_height = int(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) != 3:
        print(__doc__)
        return 1
    url, root, title = argv
    img = capture(url)
    if max_height:
        img = img.crop((0, 0, WIDTH, min(img.height, max_height)))
    os.makedirs(os.path.join(root, "web"), exist_ok=True)
    os.makedirs(os.path.join(root, "covers"), exist_ok=True)
    path = os.path.join(root, "web", title + ".fpg")
    strips = write_fpg(img, title, path)
    with open(os.path.join(root, "covers", title + ".cov"), "wb") as f:
        f.write(page_cov(img, title))
    print(f"wrote {path}: {WIDTH}x{img.height}, {strips} strips, {os.path.getsize(path) / 1048576:.1f} MB")
    print(f"wrote {os.path.join(root, 'covers', title + '.cov')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
