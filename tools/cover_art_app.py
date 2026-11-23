"""FML Cover Art: a Windows app for adding box art and album covers to an FML Launcher SD card.

  python tools/cover_art_app.py          (needs Python and Pillow)

Pick the SD card and it lists what the launcher shows: GameCube discs, Game Boy
games, apps and homebrew, web pages and music albums. Choose one to see its
cover, then search online or use your own images.

  GameCube discs   GameTDB, by game ID           -> covers/<GAMEID>.cov
  Game Boy games   libretro-thumbnails, by name  -> covers/<file name>.cov
  Apps, homebrew,
  web pages        your own images               -> covers/<file name>.cov
  Music albums     iTunes or MusicBrainz         -> folder.jpg next to the songs

A cover that gets replaced is kept in covers/old/ (folder.jpg as folder.old-<time>.jpg).
Build the .exe with tools/build_cover_art_app.bat.
"""
import ctypes
import difflib
import io
import json
import os
import queue
import re
import struct
import sys
import threading
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

from PIL import Image, ImageGrab, ImageTk

HERE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import covers  # noqa: E402

covers.FONT_PATH = os.path.join(HERE, "fonts", "Inter.ttf")

APP = "FML Cover Art"
UA = {"User-Agent": "FMLCoverArt/1.0 ( https://github.com/fixmylifedesigns/fml-launcher )"}
CACHE_DIR = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), APP)
SKIP_DIRS = {"covers", "fml", "system volume information"}
ALBUM_ART_NAMES = ["folder.jpg", "cover.jpg", "front.jpg", "albumart.jpg", "folder.png", "cover.png"]
KIND_LABEL = {"disc": "GameCube", "rom": "Game Boy", "dol": "App", "web": "Web page", "music": "Album"}
GB_LABEL = {".gb": "Game Boy", ".gbc": "Game Boy Color", ".gba": "Game Boy Advance"}
SPINE_BADGE = {"disc": "GC", "dol": "APP", "web": "WEB"}
PREVIEW_SCALE = 1.25


def http_get(url, timeout=20):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


# ---------------------------------------------------------------- reading the card

class Item:
    def __init__(self, kind, title, path, key, ext=""):
        self.kind, self.title, self.path, self.key, self.ext = kind, title, path, key, ext
        self.files = []  # songs, for an album
        self.artist = self.album = ""
        self.embedded = 0  # songs with a picture inside

    def cover_path(self, root):
        if self.kind == "music":
            for n in ALBUM_ART_NAMES:
                p = os.path.join(self.path, n)
                if os.path.exists(p):
                    return p
            return None
        p = os.path.join(root, "covers", self.key + ".cov")
        return p if os.path.exists(p) else None

    def status(self, root):
        if self.kind == "music":
            if self.embedded == len(self.files):
                return "in the MP3s"
            if self.cover_path(root):
                return os.path.basename(self.cover_path(root))
            return "in some MP3s" if self.embedded else "missing"
        return "yes" if self.cover_path(root) else "missing"


def disc_info(path):
    try:
        with open(path, "rb") as f:
            head = f.read(0x60)
    except OSError:
        return None, None
    if len(head) < 0x60 or struct.unpack(">I", head[0x1C:0x20])[0] != 0xC2339F3D:
        return None, None
    gid = head[:6].decode("ascii", "replace")
    title = head[0x20:0x60].split(b"\0")[0].decode("latin-1").strip()
    return (gid if gid.isalnum() else None), title


def synchsafe(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def id3_frames(path, want):
    """The first frame of each wanted ID3v2 id (v2.2, v2.3 and v2.4)."""
    try:
        with open(path, "rb") as f:
            head = f.read(10)
            if len(head) < 10 or head[:3] != b"ID3":
                return {}
            ver, flags, size = head[3], head[5], synchsafe(head[6:10])
            data = f.read(size)
    except OSError:
        return {}
    if flags & 0x80 and ver < 4:
        data = data.replace(b"\xff\x00", b"\xff")
    pos = 0
    if flags & 0x40 and ver >= 3:
        pos = (struct.unpack(">I", data[:4])[0] + 4) if ver == 3 else synchsafe(data[:4])
    out, hdr = {}, 6 if ver == 2 else 10
    while pos + hdr <= len(data):
        if ver == 2:
            fid, fsize = data[pos:pos + 3], int.from_bytes(data[pos + 3:pos + 6], "big")
        else:
            raw = data[pos + 4:pos + 8]
            fid, fsize = data[pos:pos + 4], synchsafe(raw) if ver == 4 else int.from_bytes(raw, "big")
        if not fid.strip(b"\0") or fsize <= 0:
            break
        fid = fid.decode("latin-1")
        if fid in want and fid not in out:
            out[fid] = data[pos + hdr:pos + hdr + fsize]
        pos += hdr + fsize
    return out


def id3_text(body):
    if not body:
        return ""
    enc, raw = body[0], body[1:]
    codec = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}.get(enc, "latin-1")
    return raw.decode(codec, "replace").split("\0")[0].strip()


def id3_picture(body, v22):
    """Image bytes from an APIC (or v2.2 PIC) frame."""
    enc = body[0]
    if v22:
        pos = 1 + 3 + 1
    else:
        pos = body.index(b"\0", 1) + 1 + 1  # mime type, then picture type
    if enc in (1, 2):
        while pos + 1 < len(body) and body[pos:pos + 2] != b"\0\0":
            pos += 2
        pos += 2
    else:
        pos = body.index(b"\0", pos) + 1
    return body[pos:]


def song_tags(path):
    f = id3_frames(path, {"TPE1", "TALB", "APIC", "TP1", "TAL", "PIC"})
    artist = id3_text(f.get("TPE1") or f.get("TP1"))
    album = id3_text(f.get("TALB") or f.get("TAL"))
    return artist, album, ("APIC" in f or "PIC" in f)


def song_picture(path):
    f = id3_frames(path, {"APIC", "PIC"})
    try:
        if "APIC" in f:
            return Image.open(io.BytesIO(id3_picture(f["APIC"], False)))
        if "PIC" in f:
            return Image.open(io.BytesIO(id3_picture(f["PIC"], True)))
    except Exception:
        pass
    return None


def scan_card(root):
    """Everything the launcher would list, plus one entry per folder of songs."""
    items, songs = [], {}

    def walk(d, depth):
        try:
            entries = sorted(os.scandir(d), key=lambda e: e.name.lower())
        except OSError:
            return
        for e in entries:
            if e.name.startswith("."):
                continue
            if e.is_dir():
                if depth < 4 and e.name.lower() not in SKIP_DIRS:
                    walk(e.path, depth + 1)
                continue
            base, ext = os.path.splitext(e.name)
            ext = ext.lower()
            if ext in (".iso", ".gcm"):
                gid, title = disc_info(e.path)
                if gid:
                    items.append(Item("disc", title or base, e.path, gid, ext))
            elif ext in covers.LIBRETRO:
                items.append(Item("rom", covers.rom_title(base), e.path, base, ext))
            elif ext == ".dol":
                if not e.name.lower().startswith("swiss"):  # the launcher hides Swiss
                    items.append(Item("dol", base, e.path, base, ext))
            elif ext == ".fpg":
                items.append(Item("web", base, e.path, base, ext))
            elif ext == ".mp3":
                songs.setdefault(d, []).append(e.path)

    walk(root, 0)
    for d, files in songs.items():
        it = Item("music", os.path.basename(d.rstrip("\\/")) or "music", d, d)
        it.files = files
        for p in files:
            artist, album, pic = song_tags(p)
            it.artist = it.artist or artist
            it.album = it.album or album
            it.embedded += pic
        if it.album:
            it.title = f"{it.artist} - {it.album}" if it.artist else it.album
        items.append(it)
    order = {"disc": 0, "rom": 1, "dol": 2, "web": 3, "music": 4}
    items.sort(key=lambda i: (order[i.kind], i.title.lower()))
    return items


def cov_images(data):
    """Front, spine and back of a .cov as PIL images (None where missing)."""
    if data[:4] != b"CCV1":
        return [None, None, None]
    out, pos = [], 4
    for _ in range(3):
        w, h = struct.unpack(">HH", data[pos:pos + 4])
        pos += 4
        if not w or not h:
            out.append(None)
            continue
        raw = data[pos:pos + w * h * 2]
        pos += w * h * 2
        img = Image.new("RGB", (w, h))
        px = img.load()
        i = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for y in range(ty, ty + 4):
                    for x in range(tx, tx + 4):
                        v = (raw[i] << 8) | raw[i + 1]
                        i += 2
                        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
                        px[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
        out.append(img)
    return out


def list_drives():
    """(root, label) for every drive, card readers and USB sticks first."""
    k32 = ctypes.windll.kernel32
    buf = ctypes.create_unicode_buffer(512)
    k32.GetLogicalDriveStringsW(512, buf)
    roots = [r for r in buf[:].split("\0") if r]
    out = []
    for r in roots:
        kind = k32.GetDriveTypeW(r)
        if kind not in (2, 3):  # removable, fixed
            continue
        name = ctypes.create_unicode_buffer(261)
        k32.GetVolumeInformationW(r, name, 261, None, None, None, None, 0)
        has_fml = os.path.isdir(os.path.join(r, "fml"))
        out.append((not has_fml, kind != 2, r, f"{r}  {name.value}" + ("  (FML card)" if has_fml else "")))
    out.sort()
    return [(r, label) for _, _, r, label in out]


# ---------------------------------------------------------------- online search
# Each search returns [(label, image, info)], where info says how to build the cover.

def get_image(url):
    try:
        return Image.open(io.BytesIO(http_get(url))).convert("RGB")
    except Exception:
        return None


def parallel(fn, args):
    """fn over args, 6 at a time, results in the same order."""
    with ThreadPoolExecutor(6) as pool:
        return list(pool.map(fn, args))


def search_gametdb(gid):
    if len(gid) < 4:
        return []
    regions = covers.REGIONS.get(gid[3], []) + [r for r in ("US", "EN", "JA") if r not in covers.REGIONS.get(gid[3], [])]
    tries = [(kind, region) for kind in covers.KINDS for region in regions]
    urls = [f"https://art.gametdb.com/wii/{k}/{r}/{gid}.png" for k, r in tries]
    imgs = parallel(get_image, urls)
    return [(f"{k} {r}", img, {"url": u}) for (k, r), u, img in zip(tries, urls, imgs) if img is not None]


_libretro_names = {}


def libretro_names(ext):
    """Every box art name for a Game Boy system (cached on the PC for a week)."""
    if ext in _libretro_names:
        return _libretro_names[ext]
    repo = covers.LIBRETRO[ext]
    cache = os.path.join(CACHE_DIR, f"{repo}.json")
    try:
        if time.time() - os.path.getmtime(cache) < 7 * 86400:
            _libretro_names[ext] = json.load(open(cache, encoding="utf-8"))
            return _libretro_names[ext]
    except OSError:
        pass
    api = f"https://api.github.com/repos/libretro-thumbnails/{repo}/git/trees/master"
    tree = json.loads(http_get(api))["tree"]
    sha = next(t["sha"] for t in tree if t["path"] == "Named_Boxarts")
    names = [t["path"][:-4] for t in json.loads(http_get(f"{api[:-6]}{sha}"))["tree"] if t["path"].endswith(".png")]
    os.makedirs(CACHE_DIR, exist_ok=True)
    json.dump(names, open(cache, "w", encoding="utf-8"))
    _libretro_names[ext] = names
    return names


def match_names(query, names, n=9):
    q = query.lower()
    words = [w for w in re.split(r"[^a-z0-9]+", covers.rom_title(q)) if w]

    def score(name):
        low = name.lower()
        s = difflib.SequenceMatcher(None, q, low).ratio()
        s += sum(1 for w in words if w in low) / max(1, len(words))
        return s + (1 if low == q else 0)

    return sorted(names, key=score, reverse=True)[:n]


def search_libretro(ext, query):
    names = match_names(query, libretro_names(ext))
    arts = parallel(lambda n: covers.download_rom_art(ext, n), names)
    return [(n, img, {"name": n, "url": url}) for n, (img, url) in zip(names, arts) if img is not None]


def search_itunes(query):
    url = "https://itunes.apple.com/search?" + urllib.parse.urlencode({"term": query, "entity": "album", "limit": 8})
    hits = [r for r in json.loads(http_get(url)).get("results", []) if r.get("artworkUrl100")]
    urls = [r["artworkUrl100"].replace("100x100bb", "600x600bb") for r in hits]
    imgs = parallel(get_image, urls)
    return [(f"{r.get('artistName', '')} - {r.get('collectionName', '')}", img, {"url": u})
            for r, u, img in zip(hits, urls, imgs) if img is not None]


def search_musicbrainz(query):
    url = "https://musicbrainz.org/ws/2/release-group/?" + urllib.parse.urlencode({"query": query, "fmt": "json", "limit": 6})
    groups = json.loads(http_get(url)).get("release-groups", [])
    urls = [f"https://coverartarchive.org/release-group/{g['id']}/front-500" for g in groups]
    imgs = parallel(get_image, urls)
    return [(f"{(g.get('artist-credit') or [{}])[0].get('name', '')} - {g.get('title', '')}", img, {"url": u})
            for g, u, img in zip(groups, urls, imgs) if img is not None]


# ---------------------------------------------------------------- making covers

def build_cov(item, front, back=None, spine=None, online=None):
    """.cov bytes for a game from the chosen images."""
    if item.kind == "rom":
        base, ext = item.key, item.ext
        size = covers.box_size(front)
        spine_size = (covers.SPINE_GB_BOX[0], size[1])
        if spine is None:
            spine = covers.make_spine(front, covers.rom_title(base), covers.PLATFORM[ext][1], spine_size)
        if back is None:
            # Screenshots for the generated back, found by the matched libretro name
            name = (online or {}).get("name", base)
            title_img, _ = covers.download_rom_art(ext, name, "Named_Titles")
            snap_img, _ = covers.download_rom_art(ext, name, "Named_Snaps")
            back = covers.rom_back(front, base, ext, title_img, snap_img)
        return covers.make_cov(front, size, spine, back, spine_size, size)
    if back is None and spine is None:
        return covers.make_cov(front)  # a wide image is read as a full wrap
    if spine is None:
        spine = covers.make_spine(front, item.title, SPINE_BADGE[item.kind], covers.SPINE)
    return covers.make_cov(front, covers.FRONT, spine, back, covers.SPINE, covers.BACK)


def album_jpeg(img):
    """A square-ish JPEG small enough for the GameCube to decode."""
    img = img.convert("RGB")
    img.thumbnail((600, 600), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90)  # baseline, not progressive
    return buf.getvalue()


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def save_game_cover(root, item, data):
    path = os.path.join(root, "covers", item.key + ".cov")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        old = os.path.join(root, "covers", "old")
        os.makedirs(old, exist_ok=True)
        os.replace(path, os.path.join(old, f"{item.key}.{stamp()}.cov"))
    with open(path, "wb") as f:
        f.write(data)
    return path


def save_album_cover(item, data):
    path = os.path.join(item.path, "folder.jpg")
    if os.path.exists(path):
        os.replace(path, os.path.join(item.path, f"folder.old-{stamp()}.jpg"))
    with open(path, "wb") as f:
        f.write(data)
    return path


# ---------------------------------------------------------------- window

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP)
        s = self.s = max(1.0, self.winfo_fpixels("1i") / 96)  # Windows display scaling
        w = min(int(1280 * s), int(self.winfo_screenwidth() * 0.94))
        h = min(int(820 * s), int(self.winfo_screenheight() * 0.88))
        self.geometry(f"{w}x{h}")
        self.minsize(int(900 * s), int(600 * s))
        self.root_dir = None
        self.items = []
        self.item = None
        self.jobs = queue.Queue()
        self.photos = []  # keeps Tk images alive
        self.result_photos = []
        self.pending = None  # (data or jpeg bytes, preview images)
        self.manual = {"front": None, "back": None, "spine": None}
        self.busy = 0

        style = ttk.Style(self)
        try:
            style.theme_use("vista")
        except tk.TclError:
            pass
        style.configure("Treeview", rowheight=int(22 * s))
        style.configure("Title.TLabel", font=("Segoe UI", 13, "bold"))
        style.configure("Dim.TLabel", foreground="#666")

        self.build_top()
        body = ttk.Frame(self)
        self.status = tk.StringVar(value="Pick your SD card.")
        ttk.Label(self, textvariable=self.status, anchor="w", relief="sunken", padding=(6, 2)).pack(fill="x", side="bottom")
        body.pack(fill="both", expand=True, padx=8, pady=(0, 4))
        self.build_list(body).pack(side="left", fill="y")
        self.build_detail(body).pack(side="left", fill="both", expand=True)

        self.after(80, self.poll)
        self.refresh_drives(select=True)

    # ---- layout
    def build_top(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="SD card:").pack(side="left")
        self.drive = ttk.Combobox(top, state="readonly", width=34)
        self.drive.pack(side="left", padx=4)
        self.drive.bind("<<ComboboxSelected>>", lambda e: self.open_card(self.drive_roots[self.drive.current()]))
        ttk.Button(top, text="Refresh", command=lambda: self.refresh_drives(select=False)).pack(side="left")
        ttk.Button(top, text="Folder...", command=self.pick_folder).pack(side="left", padx=4)
        ttk.Separator(top, orient="vertical").pack(side="left", fill="y", padx=8)
        ttk.Label(top, text="Show:").pack(side="left")
        self.filter = ttk.Combobox(top, state="readonly", width=18,
                                   values=["Everything", "Missing a cover", "GameCube", "Game Boy", "Apps", "Web pages", "Music"])
        self.filter.current(0)
        self.filter.pack(side="left", padx=4)
        self.filter.bind("<<ComboboxSelected>>", lambda e: self.fill_list())
        self.fetch_btn = ttk.Button(top, text="Get all missing game covers", command=self.fetch_all)
        self.fetch_btn.pack(side="right")

    def build_list(self, parent):
        f = ttk.Frame(parent)
        self.tree = ttk.Treeview(f, columns=("type", "cover"), show="tree headings", selectmode="browse")
        self.tree.heading("#0", text="Name")
        self.tree.heading("type", text="Type")
        self.tree.heading("cover", text="Cover")
        s = self.s
        self.tree.column("#0", width=int(210 * s))
        self.tree.column("type", width=int(105 * s), stretch=False)
        self.tree.column("cover", width=int(75 * s), stretch=False)
        self.tree.tag_configure("missing", foreground="#b3261e")
        sb = ttk.Scrollbar(f, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.select())
        return f

    def build_detail(self, parent):
        f = ttk.Frame(parent, padding=(10, 0, 0, 0))
        self.name_var = tk.StringVar()
        self.info_var = tk.StringVar()
        ttk.Label(f, textvariable=self.name_var, style="Title.TLabel").pack(anchor="w")
        info = ttk.Label(f, textvariable=self.info_var, style="Dim.TLabel", justify="left")
        info.pack(anchor="w", fill="x", pady=(0, 6))
        info.bind("<Configure>", lambda e: info.configure(wraplength=e.width))

        s = self.s
        prev = ttk.Frame(f)
        prev.pack(fill="x")
        prev.columnconfigure(0, weight=1, uniform="p")
        prev.columnconfigure(1, weight=1, uniform="p")
        self.cur_box = ttk.LabelFrame(prev, text="On the card", padding=6)
        self.cur_box.grid(row=0, column=0, sticky="nsew")
        self.new_box = ttk.LabelFrame(prev, text="New", padding=6)
        self.new_box.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        self.cur_canvas = tk.Canvas(self.cur_box, width=10, height=int(250 * s), highlightthickness=0, bg="#2b2b30")
        self.cur_canvas.pack(fill="both", expand=True)
        self.new_canvas = tk.Canvas(self.new_box, width=10, height=int(250 * s), highlightthickness=0, bg="#2b2b30")
        self.new_canvas.pack(fill="both", expand=True)

        actions = ttk.Frame(f, padding=(0, 8))
        actions.pack(fill="x")
        self.save_btn = ttk.Button(actions, text="Save to card", command=self.save, state="disabled")
        self.save_btn.pack(side="right")
        self.clear_btn = ttk.Button(actions, text="Clear", command=self.clear_pending)
        self.clear_btn.pack(side="right", padx=4)

        nb = ttk.Notebook(f)
        nb.pack(fill="both", expand=True)
        self.nb = nb

        online = ttk.Frame(nb, padding=8)
        nb.add(online, text="Search online")
        row = ttk.Frame(online)
        row.pack(fill="x")
        self.query = tk.StringVar()
        self.query_entry = ttk.Entry(row, textvariable=self.query)
        self.query_entry.pack(side="left", fill="x", expand=True)
        self.query_entry.bind("<Return>", lambda e: self.search())
        self.source = ttk.Combobox(row, state="readonly", width=16)
        self.source.pack(side="left", padx=4)
        self.search_btn = ttk.Button(row, text="Search", command=self.search)
        self.search_btn.pack(side="left")
        self.search_note = tk.StringVar()
        ttk.Label(online, textvariable=self.search_note, style="Dim.TLabel").pack(anchor="w", pady=(4, 2))
        wrap = ttk.Frame(online)
        wrap.pack(fill="both", expand=True)
        self.results = tk.Canvas(wrap, height=int(175 * s), highlightthickness=0)
        rsb = ttk.Scrollbar(wrap, orient="horizontal", command=self.results.xview)
        self.results.configure(xscrollcommand=rsb.set)
        self.results.pack(fill="both", expand=True)
        rsb.pack(fill="x")
        self.results_inner = ttk.Frame(self.results)
        self.results.create_window(0, 0, window=self.results_inner, anchor="nw")
        self.results_inner.bind("<Configure>", lambda e: self.results.configure(scrollregion=self.results.bbox("all")))

        manual = ttk.Frame(nb, padding=8)
        nb.add(manual, text="Your own images")
        self.manual_vars = {}
        for part, label in (("front", "Front (or a full wrap: back | spine | front)"),
                            ("back", "Back (optional)"), ("spine", "Spine (optional)")):
            r = ttk.Frame(manual)
            r.pack(fill="x", pady=3)
            ttk.Label(r, text=label, width=40).pack(side="left")
            v = tk.StringVar(value="-")
            self.manual_vars[part] = v
            ttk.Button(r, text="Choose...", command=lambda p=part: self.choose_image(p)).pack(side="left")
            ttk.Button(r, text="Paste", command=lambda p=part: self.paste_image(p)).pack(side="left", padx=4)
            ttk.Label(r, textvariable=v, style="Dim.TLabel").pack(side="left", padx=6)
        self.manual_note = tk.StringVar()
        ttk.Label(manual, textvariable=self.manual_note, style="Dim.TLabel", wraplength=600, justify="left").pack(anchor="w", pady=(8, 0))
        return f

    # ---- card
    def refresh_drives(self, select):
        drives = list_drives()
        self.drive_roots = [r for r, _ in drives]
        self.drive["values"] = [label for _, label in drives]
        if self.root_dir in self.drive_roots:
            self.drive.current(self.drive_roots.index(self.root_dir))
        elif select and drives and "(FML card)" in drives[0][1]:
            self.drive.current(0)
            self.open_card(drives[0][0])
        if not select and self.root_dir:
            self.open_card(self.root_dir)

    def pick_folder(self):
        d = filedialog.askdirectory(title="SD card or a copy of it")
        if d:
            self.open_card(os.path.normpath(d) + os.sep)

    def open_card(self, root):
        self.root_dir = root
        if root not in getattr(self, "drive_roots", []):
            self.drive.set(root)
        self.status.set(f"Reading {root} ...")
        self.run(lambda: scan_card(root), self.card_read)

    def card_read(self, items):
        self.items = items
        self.fill_list()
        missing = sum(1 for i in items if i.status(self.root_dir) == "missing")
        self.status.set(f"{self.root_dir}: {len(items)} items, {missing} without a cover.")

    def fill_list(self):
        keep = self.item
        self.tree.delete(*self.tree.get_children())
        f = self.filter.get()
        want = {"GameCube": "disc", "Game Boy": "rom", "Apps": "dol", "Web pages": "web", "Music": "music"}.get(f)
        groups = {}
        for idx, it in enumerate(self.items):
            st = it.status(self.root_dir)
            if want and it.kind != want or f == "Missing a cover" and st != "missing":
                continue
            g = KIND_LABEL[it.kind] if it.kind != "dol" else "Apps & homebrew"
            if g not in groups:
                groups[g] = self.tree.insert("", "end", text=g + ("s" if g in ("GameCube", "Album", "Web page") else ""), open=True)
            kind = GB_LABEL.get(it.ext, KIND_LABEL[it.kind])
            self.tree.insert(groups[g], "end", iid=str(idx), text=it.title, values=(kind, st),
                             tags=("missing",) if st == "missing" else ())
        if keep in self.items and self.tree.exists(str(self.items.index(keep))):
            self.tree.selection_set(str(self.items.index(keep)))
            self.tree.see(str(self.items.index(keep)))

    # ---- one item
    def select(self):
        sel = self.tree.selection()
        if not sel or not sel[0].isdigit():
            return
        it = self.items[int(sel[0])]
        if it is self.item:
            return
        self.item = it
        self.clear_pending()
        self.clear_results()
        self.name_var.set(it.title)
        rel = os.path.relpath(it.path, self.root_dir)
        if it.kind == "music":
            info = f"{rel}  -  {len(it.files)} song(s). Saved as folder.jpg next to the songs."
            if it.embedded:
                info += f"\n{it.embedded} of them have a picture inside the MP3, which the launcher shows first."
        elif it.kind == "disc":
            info = f"{rel}  -  game ID {it.key}. Saved as covers\\{it.key}.cov"
        else:
            info = f"{rel}. Saved as covers\\{it.key}.cov"
        self.info_var.set(info)
        self.show_current()

        sources = {"disc": ["GameTDB"], "rom": ["libretro"], "music": ["iTunes", "MusicBrainz"]}.get(it.kind, [])
        self.source["values"] = sources
        if sources:
            self.source.current(0)
            self.query.set({"disc": it.key, "rom": it.key}.get(it.kind) or
                           (f"{it.artist} {it.album}".strip() or it.title))
            self.search_note.set({"disc": "Search by game ID.", "rom": "Search by name; the closest box art names are shown.",
                                  "music": "Search by artist and album."}[it.kind])
            for w in (self.query_entry, self.search_btn):
                w.state(["!disabled"])
            self.nb.select(0)
        else:
            self.query.set("")
            self.search_note.set("There's no online database for apps, homebrew and web pages. Use your own images.")
            for w in (self.query_entry, self.search_btn):
                w.state(["disabled"])
            self.nb.select(1)
        self.manual_note.set(
            "One picture is cropped to a square album cover." if it.kind == "music" else
            "Game Boy covers get a spine and back made to match if you only give a front."
            if it.kind == "rom" else
            "Only a front: the case gets a plain spine and back. A wide image is split as a full wrap.")

    def show_current(self):
        it = self.item
        p = it.cover_path(self.root_dir)
        if it.kind == "music":
            img = None
            for song in it.files:
                img = song_picture(song)
                if img:
                    break
            if img is None and p:
                try:
                    img = Image.open(p)
                except Exception:
                    img = None
            self.draw(self.cur_canvas, [img] if img else [], "no album cover")
        elif p:
            self.draw(self.cur_canvas, cov_images(open(p, "rb").read()), "")
        else:
            self.draw(self.cur_canvas, [], "no cover yet")

    def draw(self, canvas, images, empty):
        canvas.delete("all")
        canvas.update_idletasks()
        cw, ch = max(canvas.winfo_width(), 200), max(canvas.winfo_height(), 200)
        images = [im for im in images if im is not None]
        if not images:
            canvas.create_text(cw / 2, ch / 2, text=empty, fill="#aaa", font=("Segoe UI", 11))
            return
        if self.item and self.item.kind == "music":
            im = images[0].convert("RGB")
            im.thumbnail((ch - 20, ch - 20), Image.LANCZOS)
            parts = [im]
        else:
            # front | spine | back, scaled to fit the box
            total = sum(im.width for im in images) + 8 * (len(images) - 1)
            s = min(PREVIEW_SCALE * self.s, (cw - 16) / total, (ch - 16) / max(im.height for im in images))
            parts = [im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.LANCZOS) for im in images]
        x = (cw - (sum(p.width for p in parts) + 8 * (len(parts) - 1))) / 2
        for p in parts:
            photo = ImageTk.PhotoImage(p)
            self.photos.append(photo)
            canvas.create_image(x, (ch - p.height) / 2, image=photo, anchor="nw")
            x += p.width + 8
        self.photos = self.photos[-12:]

    # ---- search
    def clear_results(self):
        for w in self.results_inner.winfo_children():
            w.destroy()
        self.result_photos = []

    def search(self):
        it, q, src = self.item, self.query.get().strip(), self.source.get()
        if not it or not q:
            return
        self.clear_results()
        self.search_note.set(f"Searching {src} ...")

        def work():
            if it.kind == "disc":
                return search_gametdb(q.upper()[:6])
            if it.kind == "rom":
                return search_libretro(it.ext, q)
            return search_itunes(q) if src == "iTunes" else search_musicbrainz(q)

        def done(found):
            if it is not self.item:
                return
            self.search_note.set(f"{len(found)} found. Click one to use it." if found else "Nothing found. Try other words.")
            for label, img, info in found:
                thumb = img.copy()
                thumb.thumbnail((int(130 * self.s), int(130 * self.s)), Image.LANCZOS)
                photo = ImageTk.PhotoImage(thumb)
                self.result_photos.append(photo)
                cell = ttk.Frame(self.results_inner, padding=4)
                cell.pack(side="left", anchor="n")
                b = tk.Button(cell, image=photo, relief="flat", cursor="hand2", bd=0,
                              command=lambda i=img, inf=info: self.use_online(i, inf))
                b.pack()
                ttk.Label(cell, text=label[:28], style="Dim.TLabel").pack()

        self.run(work, done, error=lambda e: self.search_note.set(f"Search failed: {e}"))

    def use_online(self, img, info):
        self.make_pending(img, online=info)

    # ---- own images
    def set_manual(self, part, img, label):
        self.manual[part] = img.convert("RGB")
        self.manual_vars[part].set(label)
        if self.manual["front"] is not None:
            self.make_pending(self.manual["front"], back=self.manual["back"], spine=self.manual["spine"])

    def choose_image(self, part):
        if not self.item:
            return
        p = filedialog.askopenfilename(title=f"{part.title()} image",
                                       filetypes=[("Images", "*.png *.jpg *.jpeg *.webp *.bmp *.gif"), ("All files", "*.*")])
        if p:
            try:
                self.set_manual(part, Image.open(p), os.path.basename(p))
            except Exception as e:
                messagebox.showerror(APP, f"Couldn't open that image:\n{e}")

    def paste_image(self, part):
        if not self.item:
            return
        clip = ImageGrab.grabclipboard()
        if isinstance(clip, list) and clip:
            try:
                clip = Image.open(clip[0])
            except Exception:
                clip = None
        if not isinstance(clip, Image.Image):
            messagebox.showinfo(APP, "There's no picture on the clipboard. Right-click an image in your browser and choose Copy image.")
            return
        self.set_manual(part, clip, "pasted")

    # ---- pending cover
    def make_pending(self, front, back=None, spine=None, online=None):
        it = self.item
        self.status.set("Making the cover ...")

        def work():
            if it.kind == "music":
                data = album_jpeg(front)
                return data, [Image.open(io.BytesIO(data))]
            data = build_cov(it, front, back, spine, online)
            return data, cov_images(data)

        def done(res):
            if it is not self.item:
                return
            self.pending = res
            self.draw(self.new_canvas, res[1], "")
            self.save_btn.state(["!disabled"])
            self.status.set("Check the new cover, then Save to card.")

        self.run(work, done)

    def clear_pending(self):
        self.pending = None
        self.manual = {"front": None, "back": None, "spine": None}
        for v in getattr(self, "manual_vars", {}).values():
            v.set("-")
        self.draw(self.new_canvas, [], "search or choose an image")
        self.save_btn.state(["disabled"])

    def save(self):
        it, pending = self.item, self.pending
        if not it or not pending:
            return
        try:
            path = save_album_cover(it, pending[0]) if it.kind == "music" else save_game_cover(self.root_dir, it, pending[0])
        except OSError as e:
            messagebox.showerror(APP, f"Couldn't write to the card:\n{e}")
            return
        self.status.set(f"Saved {path}")
        self.clear_pending()
        self.show_current()
        self.fill_list()

    # ---- all missing
    def fetch_all(self):
        todo = [i for i in self.items if i.kind in ("disc", "rom") and i.status(self.root_dir) == "missing"]
        if not todo:
            messagebox.showinfo(APP, "Every GameCube and Game Boy game already has a cover.")
            return
        if not messagebox.askyesno(APP, f"Download covers for {len(todo)} game(s) without one?"):
            return
        self.fetch_btn.state(["disabled"])
        root = self.root_dir

        def work():
            got, miss = 0, []
            for n, it in enumerate(todo, 1):
                self.jobs.put((self.status.set, f"Getting covers {n}/{len(todo)}: {it.title}"))
                if it.kind == "disc":
                    img, url = covers.download(it.key)
                    data = covers.make_cov(img) if img is not None else None
                else:
                    img, url = covers.download_rom_art(it.ext, it.key)
                    data = covers.rom_cov(img, it.ext, it.key) if img is not None else None
                if data:
                    save_game_cover(root, it, data)
                    got += 1
                else:
                    miss.append(it.title)
            return got, miss

        def done(res):
            got, miss = res
            self.fetch_btn.state(["!disabled"])
            self.fill_list()
            if self.item:
                self.show_current()
            msg = f"Saved {got} cover(s)."
            if miss:
                msg += "\n\nNot found online (search by hand or use your own images):\n" + "\n".join(miss[:20])
            self.status.set(f"Saved {got} cover(s), {len(miss)} not found.")
            messagebox.showinfo(APP, msg)

        self.run(work, done, error=lambda e: (self.fetch_btn.state(["!disabled"]), self.status.set(f"Stopped: {e}")))

    # ---- background work
    def run(self, work, done, error=None):
        self.busy += 1
        self.config(cursor="watch")

        def thread():
            try:
                res = work()
                self.jobs.put((done, res))
            except Exception as e:
                self.jobs.put((error or (lambda err: self.status.set(f"Error: {err}")), e))
            self.jobs.put((self.job_done, None))

        threading.Thread(target=thread, daemon=True).start()

    def job_done(self, _):
        self.busy -= 1
        if self.busy <= 0:
            self.busy = 0
            self.config(cursor="")

    def poll(self):
        try:
            while True:
                fn, arg = self.jobs.get_nowait()
                fn(arg)
        except queue.Empty:
            pass
        self.after(80, self.poll)


if __name__ == "__main__":
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled screens
    except Exception:
        pass
    App().mainloop()
