"""Convert the boot splash video for FML Launcher.

  python tools/make_video.py design/splash/splash.mp4 D:/fml/splash.fmv

Frames are scaled to 512x384 and stored as GameCube CMPR textures (DXT1,
4 bits per pixel) so the console can show each frame straight from the file,
with no decoding. The launcher streams it from the SD card at boot.

.fmv layout (big endian): "FMV1", u16 width, u16 height, u16 fps, u16 frames,
u32 bytes per frame, padding to 32 bytes, then the frames.
"""
import os
import struct
import subprocess
import sys

import numpy as np

W, H, FPS = 512, 384, 24


def read_frames(path):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-vf", f"fps={FPS},scale={W}:{H}:flags=lanczos",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, H, W, 3)


def to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.uint32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)


def from565(v):
    r = ((v >> 11) & 31) * 255 / 31
    g = ((v >> 5) & 63) * 255 / 63
    b = (v & 31) * 255 / 31
    return np.stack([r, g, b], -1)


WEIGHTS = np.array([[1, 0], [0, 1], [2 / 3, 1 / 3], [1 / 3, 2 / 3]], np.float32)  # p0, p1 share per index


def fit_indices(blocks, c0, c1):
    """Best palette index for each pixel, given 565 endpoints (c0 > c1: 4-colour mode)."""
    p0, p1 = from565(c0), from565(c1)
    palette = WEIGHTS[:, 0, None] * p0[..., None, :] + WEIGHTS[:, 1, None] * p1[..., None, :]  # ...,4,3
    dist = ((blocks[..., :, None, :] - palette[..., None, :, :]) ** 2).sum(-1)
    return dist.argmin(-1), dist.min(-1).sum(-1)


def order(c0, c1):
    swap = c0 < c1
    return np.where(swap, c1, c0), np.where(swap, c0, c1)


def encode_cmpr(frame):
    """One RGB frame -> CMPR bytes (8x8 tiles of four DXT1 blocks).

    Endpoints start at the ends of the block's main colour axis (PCA), then
    are refined by least squares against the chosen palette indices."""
    h, w = frame.shape[:2]
    bh, bw = h // 4, w // 4
    blocks = frame.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 3).astype(np.float32)

    # Main colour axis by power iteration on each block's covariance
    mean = blocks.mean(2, keepdims=True)
    d = blocks - mean
    cov = np.einsum("...ki,...kj->...ij", d, d)
    axis = blocks.max(2) - blocks.min(2) + 1e-3
    for _ in range(6):
        axis = np.einsum("...ij,...j->...i", cov, axis)
        axis /= np.linalg.norm(axis, axis=-1, keepdims=True) + 1e-9
    proj = np.einsum("...ki,...i->...k", d, axis)
    m = mean[..., 0, :]
    hi = m + proj.max(-1, keepdims=True) * axis
    lo = m + proj.min(-1, keepdims=True) * axis
    c0, c1 = order(to565(hi), to565(lo))
    idx, err = fit_indices(blocks, c0, c1)

    # Least-squares refinement
    for _ in range(3):
        wt = WEIGHTS[idx]                       # bh,bw,16,2
        a, b = wt[..., 0], wt[..., 1]
        aa, bb, ab = (a * a).sum(-1), (b * b).sum(-1), (a * b).sum(-1)
        ax = (a[..., None] * blocks).sum(2)
        bx = (b[..., None] * blocks).sum(2)
        det = aa * bb - ab * ab
        ok = np.abs(det) > 1e-6
        det = np.where(ok, det, 1)
        e0 = (bb[..., None] * ax - ab[..., None] * bx) / det[..., None]
        e1 = (aa[..., None] * bx - ab[..., None] * ax) / det[..., None]
        n0, n1 = order(to565(e0), to565(e1))
        nidx, nerr = fit_indices(blocks, n0, n1)
        better = ok & (nerr < err)
        c0, c1 = np.where(better, n0, c0), np.where(better, n1, c1)
        idx, err = np.where(better[..., None], nidx, idx), np.where(better, nerr, err)

    idx[c0 == c1] = 0  # flat block (3-colour mode would make index 3 transparent)

    rows = idx.reshape(bh, bw, 4, 4)
    packed = (rows[..., 0] << 6) | (rows[..., 1] << 4) | (rows[..., 2] << 2) | rows[..., 3]
    block_bytes = np.concatenate([
        (c0 >> 8)[..., None], (c0 & 255)[..., None], (c1 >> 8)[..., None], (c1 & 255)[..., None],
        packed], -1).astype(np.uint8)                                       # bh,bw,8

    # 8x8 tiles: top-left, top-right, bottom-left, bottom-right blocks
    t = block_bytes.reshape(bh // 2, 2, bw // 2, 2, 8).transpose(0, 2, 1, 3, 4)
    return t.tobytes()


def main(src, dst):
    frames = read_frames(src)
    data = [encode_cmpr(f) for f in frames]
    frame_bytes = len(data[0])
    header = b"FMV1" + struct.pack(">4HI", W, H, FPS, len(data), frame_bytes)
    header += b"\0" * (32 - len(header))
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(header)
        for d in data:
            f.write(d)
    total = 32 + frame_bytes * len(data)
    print(f"wrote {dst}: {len(data)} frames at {FPS} fps, {W}x{H}, {total / 1048576:.1f} MB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], sys.argv[2])
