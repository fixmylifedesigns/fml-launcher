#!/usr/bin/env python3
"""Converts any video into something the FML Launcher movie player can play.

The GameCube is too slow for modern video formats, so the movie player plays
MPEG-1 video with MP2 sound (.mpg), the format of Video CDs. This script uses
ffmpeg (https://ffmpeg.org, must be installed or next to this script) to make
one:

    python tools/make_movie.py "Holiday 2004.mp4"
    python tools/make_movie.py *.mkv --out E:\\videos
    python tools/make_movie.py film.mp4 --width 400 --fps 25

Smaller pictures play more smoothly: 320 pixels wide is the safe default,
400 usually works. Copy the .mpg files anywhere on the SD card.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

MPEG1_RATES = (23.976, 24, 25, 29.97, 30)


def find_ffmpeg():
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ('ffmpeg', 'ffmpeg.exe'):
        local = os.path.join(here, name)
        if os.path.isfile(local):
            return local
    found = shutil.which('ffmpeg')
    if not found:
        sys.exit('ffmpeg not found. Install it from https://ffmpeg.org and try again.')
    return found


def convert(ffmpeg, src, out_dir, width, fps, video_kbps):
    base = os.path.splitext(os.path.basename(src))[0]
    dst = os.path.join(out_dir or os.path.dirname(os.path.abspath(src)), base + '.mpg')
    if os.path.abspath(dst) == os.path.abspath(src):
        dst = os.path.join(os.path.dirname(dst), base + ' (GameCube).mpg')
    cmd = [
        ffmpeg, '-hide_banner', '-loglevel', 'error', '-stats', '-y', '-i', src,
        # Width multiple of 16 (the decoder's block size), height even, square pixels
        '-vf', 'scale=%d:-2:flags=lanczos,setsar=1' % (width // 16 * 16),
        '-r', str(fps),
        '-c:v', 'mpeg1video', '-b:v', '%dk' % video_kbps, '-maxrate', '%dk' % (video_kbps * 2),
        '-bufsize', '%dk' % (video_kbps * 2), '-g', str(int(fps)),
        '-c:a', 'mp2', '-b:a', '160k', '-ar', '44100', '-ac', '2',
        '-f', 'mpeg', dst,
    ]
    print('%s -> %s' % (src, dst))
    if subprocess.call(cmd) != 0:
        print('  failed')
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('videos', nargs='+', help='video files (wildcards work)')
    ap.add_argument('--out', help='folder for the .mpg files (default: next to each video)')
    ap.add_argument('--width', type=int, default=320, help='picture width in pixels (default 320)')
    ap.add_argument('--fps', type=float, default=24, choices=MPEG1_RATES, help='frames per second (default 24)')
    ap.add_argument('--kbps', type=int, default=1000, help='video bitrate (default 1000)')
    args = ap.parse_args()

    ffmpeg = find_ffmpeg()
    files = []
    for pattern in args.videos:
        files += glob.glob(pattern) or [pattern]
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    ok = sum(convert(ffmpeg, f, args.out, args.width, args.fps, args.kbps) for f in files)
    print('%d of %d converted' % (ok, len(files)))


if __name__ == '__main__':
    main()
