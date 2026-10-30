# FML Launcher

A GameCube launcher with a 3D carousel UI, meant to be flashed onto a
[PicoLoader](https://github.com/makeo/PicoLoader) modchip instead of Swiss.

It scans the SD card (SD2SP2, SD Gecko in slot B, or slot A) for every `.dol`,
`.iso`/`.gcm` and Game Boy ROM (`.gb`, `.gbc`, `.gba`) in any folder and shows them as 3D
game cases (Game Boy games as Game Boy boxes). Game Boy ROMs are started with Visual Boy
Advance GX (`vbagx*.dol` on the card), passing the ROM's folder and file name. Homebrew `.dol`s boot
directly. Disc images are handed to Swiss, which is started with
`Autoload=<path>` and the environment variable `CUBEBOOT=1` (the mode Swiss uses when another
launcher starts it), so Swiss shows none of its menus and boots the game straight away. That needs a
`swiss*.dol` somewhere on the card. Swiss itself isn't shown in the carousel; open it
from Settings (Y).

## Features
- 3D cover-flow shelf with real box art (front, spine and back), Game Boy games in Game Boy boxes.
- Tabs: All, Recent, Favourites, GameCube, Game Boy, Apps, plus search.
- Continue: opens on the last game you played. Play counts and "played 2 days ago" are saved to
  `fml/history.txt`.
- 480p progressive (component / Digital AV) and a 16:9 widescreen layout, set in Settings
  (`fml/settings.ini`). Swiss is told the same video mode when it starts a game.
- Fast startup: the game list is saved to `fml/library.cache`, so only new files are read.
- Updates without reflashing: if `fml/launcher.dol` on the card is a different build, the
  launcher in the Pico runs it instead. Copy a new `cubelaunch.dol` there to update.
- Boot video streamed from `fml/splash.fmv`.
- Disc drive: an original GameCube disc in the drive shows up first on the shelf. Press A for
  **Play disc** (started through Swiss with your video settings), **Copy to SD card** (a full
  1.36 GB backup to `games/<Title> [GAMEID].iso`, with a CRC32 to check on redump.org), or
  **Play the SD card copy** once it's there. Copies are written as `.part` and only renamed
  when complete.
- Music Player app (Apps tab): plays `.mp3` files from anywhere on the card in a Windows XP
  media player style window: playlist, ID3 tags, frequency bars, shuffle, repeat, seek and
  volume. Music keeps playing on the shelf after you close it.
  Album covers come from the picture in the MP3, or a `Folder.jpg` / `cover.jpg` next to it.
  The font has no Japanese, so an `fml-titles.txt` next to the songs can give them readable
  titles, one `file name.mp3 = Title` per line (UTF-8).
- Movie Player app: plays MPEG-1 `.mpg` videos with sound. Other formats are listed as
  "needs converting": run `python tools/make_movie.py video.mp4` (needs ffmpeg) to make one.
  Videos are shown by folder, so TV shows can go in
  `videos/TV Shows/<Show>/Season 1/<Show> S01E01 - Title.mpg`; episodes sort by number.

## Controls
| Button | Action |
|---|---|
| D-pad / stick left-right | Slide through games |
| L / R | Switch tab |
| Z | Search: on-screen keyboard, the carousel filters as you type (B deletes, OK/Z closes) |
| A | Start / Play |
| X | Star / unstar a game (Favourites) |
| Y | Settings: video mode, screen shape, Open Swiss, Switch SD card, Restart |
| START | Shortcut: restart (hot reset) |
| Hold R while starting | Use the launcher in the Pico, not `fml/launcher.dol` |
| Hold B while starting | Force 480i (if 480p shows no picture) |
| Hold Z while starting | Debug: open the demo card even with an SD card in |
| Y on "No SD card" | Demo mode |

Music Player: A play/pause, L / R previous/next song, C-stick left-right seek, C-stick up-down
volume, X shuffle, Y repeat, Z stop, B back to the shelf (the music keeps playing).
Movie Player: A play/pause, L / R skip 30 s, C-stick (or stick / D-pad) seek and volume, B back.

## Covers
Each game shows the best art it has:
1. `covers/<KEY>.cov` on the card, with front, spine and back. KEY is the game ID for discs
   (e.g. `GALE01`) or the `.dol` name without extension (e.g. `Snes9xGX`).
2. Otherwise, a generated cover in a colour picked from the name. Discs show their built-in
   banner on it; homebrew shows its initials.

Make `.cov` files on your PC with `tools/covers.py` (needs Python and Pillow):

```
python tools/covers.py fetch E:\                  # every game on the card, from GameTDB
python tools/covers.py add E:\ GALE01 wrap.jpg    # your own scan (full wrap or front only)
python tools/covers.py add E:\ Snes9xGX icon.png  # art for a .dol
```

A `.png`/`.jpg` with the same name as a `.dol` is picked up by `fetch` too.

## Build
Install devkitPro with the GameCube packages (`gamecube-dev`), then from a devkitPro MSYS2 shell:

```
make
```

This gives you `cubelaunch.dol`.

## Testing in Dolphin
Dolphin can't emulate GameCube SD adapters, so use demo mode: press **Y** on the "No SD card"
screen, or build with `make DEMO=1` to go straight in. Demo mode shows a fake card. Covers baked
into the build with

```
python tools/covers.py embed source/demo_covers.c C:\gc-sd\covers\*.cov
```

appear on it, so you can see real art in Dolphin. Launching in demo mode only plays the animation.

`C:\gc-sd` holds a ready-made test card: Swiss, copies of `tools/testapp` (a tiny DOL that says
it was booted), and a deliberately broken `.dol`. Copy it onto a real SD card to test on the console.

## Put it on the PicoLoader
```
python tools/make_uf2.py cubelaunch.dol cubelaunch.uf2
```
(or use the [PicoLoader converter](https://makeo.github.io/PicoLoader/converter/)), then hold
BOOTSEL on the Pico and copy the `.uf2` to it. After that, copy `cubelaunch.dol` to
`fml/launcher.dol` on the SD card to update; no reflashing needed.

Ready-made builds are on the [releases page](https://github.com/fixmylifedesigns/fml-launcher/releases).

## Code map
- `source/main.c`: screens, input, UI layout and theme colours, launching.
- `source/scene.c`: the 3D carousel (case geometry, layout, easing, reflections, floor).
- `source/covers.c`: loads `.cov` files, generates covers and reads disc banners.
- `source/library.c`: SD scanning (games, music and videos) and the demo card.
- `source/disc.c`: disc drive thread: detects discs and copies them to the card.
- `source/music.c`, `source/movie.c`, `source/xpui.c`: the music and movie players and their
  XP-style look. `source/plm.c` builds `pl_mpeg.h` (MIT, phoboslab) for MPEG-1 decoding. `source/stbi.c` builds `stb_image.h` (public domain) for album covers.
- `tools/make_movie.py` converts any video to the `.mpg` the movie player plays.
- `source/boot.c` + `stub.S`: loads a DOL, passes argv (for Swiss autoload) and jumps to it.
- `source/gx.c`: GPU setup, textures, GPU text (font atlas) and rounded shapes.
- `source/gfx.c`: software drawing, used for the background and generated covers.
- `design/mockup.html`: the HTML design at real 640×480 size.
- `tools/make_font.py` turns Inter into `source/font_data.c`.
- `tools/make_splash.py` bakes the boot splash (`design/splash/splash_a.png`, made in Higgsfield) into `source/splash_data.c`.
- `tools/make_uf2.py` turns `cubelaunch.dol` into the PicoLoader `.uf2`.
- `tools/make_video.py` turns an mp4 into the boot video (`fml/splash.fmv` on the SD card; the
  demo build embeds `data/splash.fmv`). The Pico's flash is too small for it, so it lives on the card.
