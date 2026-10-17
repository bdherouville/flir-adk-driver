#!/usr/bin/env python3
"""
flir_test.py — capture + calibration test for FLIR ADK / Boson 640

Grabs frames in AGC8 and RAW16 modes, writes PNGs, prints calibration stats.

Usage: python3 tools/flir_test.py [-d /dev/video5] [-c /dev/ttyACM0] [-o output_dir]
"""

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path
from datetime import datetime
from PIL import Image, ImageDraw, ImageFont

W, H = 640, 512


def grab_raw(flir_grab, dev, cci, mode, nframes=1):
    cmd = [flir_grab, "-d", dev, "-m", mode, "-n", str(nframes)]
    if cci:
        cmd += ["-c", cci]
    r = subprocess.run(cmd, capture_output=True, timeout=30)
    return r.stdout, r.stderr.decode(errors="replace")


def yuv420_to_image(yuv_data):
    """Convert I420/YUV420 to grayscale via ffmpeg — Boson uses full YUV range"""
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
        tmp = f.name
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo",
         "-pix_fmt", "yuv420p", "-s", f"{W}x{H}", "-i", "pipe:0",
         "-frames:v", "1", "-pix_fmt", "gray", tmp],
        input=yuv_data, capture_output=True
    )
    img = Image.open(tmp).copy()
    os.unlink(tmp)
    return img


def raw16_to_gray(data):
    n = W * H
    px = struct.unpack(f"<{n}H", data[:n * 2])
    lo, hi = min(px), max(px)
    if hi == lo:
        return bytes([128] * n), lo, hi
    rng = hi - lo
    gray = bytes(((v - lo) * 255 // rng) for v in px)
    return gray, lo, hi


def ironbow_lut():
    lut = []
    for i in range(256):
        t = i / 255.0
        if t < 0.25:
            s = t / 0.25
            lut.append((0, 0, int(128 * s)))
        elif t < 0.5:
            s = (t - 0.25) / 0.25
            lut.append((int(200 * s), 0, int(128 * (1 - s))))
        elif t < 0.75:
            s = (t - 0.5) / 0.25
            lut.append((200 + int(55 * s), int(200 * s), 0))
        else:
            s = (t - 0.75) / 0.25
            lut.append((255, 200 + int(55 * s), int(255 * s)))
    return lut


def colorize(gray_bytes, lut):
    rgb = bytearray(len(gray_bytes) * 3)
    for i, g in enumerate(gray_bytes):
        r, gc, b = lut[g]
        o = i * 3
        rgb[o] = r; rgb[o + 1] = gc; rgb[o + 2] = b
    return bytes(rgb)


def get_font():
    for path in [
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    ]:
        try:
            return ImageFont.truetype(path, 14)
        except (IOError, OSError):
            continue
    return ImageFont.load_default()


def annotate(img, text):
    draw = ImageDraw.Draw(img)
    font = get_font()
    y = 5
    for line in text.split("\n"):
        draw.text((6, y + 1), line, fill=(0, 0, 0), font=font)
        draw.text((5, y), line, fill=(255, 255, 255), font=font)
        y += 18


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-d", "--device", default="/dev/video5")
    p.add_argument("-c", "--cci", default="/dev/ttyACM0")
    p.add_argument("-o", "--outdir", default="output")
    p.add_argument("-n", "--frames", type=int, default=5)
    p.add_argument("--grab", default="./flir_grab")
    args = p.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    lut = ironbow_lut()

    if not os.path.exists(args.grab):
        sys.exit("Build flir_grab first: make")

    cci = args.cci if os.path.exists(args.cci) else None

    # --- AGC8 capture ---
    print(f"[AGC8] Capturing {args.frames} frames...")
    raw, info = grab_raw(args.grab, args.device, cci, "agc8", args.frames)
    if info.strip():
        print(info.strip())

    frame_sz = W * H * 3 // 2
    n_agc = len(raw) // frame_sz
    for i in range(n_agc):
        chunk = raw[i * frame_sz:(i + 1) * frame_sz]
        img_g = yuv420_to_image(chunk)
        # Colorize AGC8 with iron-bow too
        img_c = Image.frombytes("RGB", (W, H), colorize(img_g.tobytes(), lut))
        label = info.strip().split("\n")[0] if info.strip() else ""
        annotate(img_g.convert("RGB"), f"AGC8 #{i}  {W}x{H}\n{label}")
        annotate(img_c, f"AGC8 iron #{i}\n{label}")
        fname_g = outdir / f"agc8_gray_{ts}_{i:03d}.png"
        fname_c = outdir / f"agc8_iron_{ts}_{i:03d}.png"
        img_g.save(fname_g)
        img_c.save(fname_c)
        print(f"  {fname_g}  {fname_c}")

    # --- RAW16 capture ---
    print(f"\n[RAW16] Capturing {args.frames} frames...")
    raw16, info16 = grab_raw(args.grab, args.device, cci, "raw16", args.frames)
    if info16.strip():
        print(info16.strip())

    frame_sz_16 = W * H * 2
    n_raw = len(raw16) // frame_sz_16
    for i in range(n_raw):
        chunk = raw16[i * frame_sz_16:(i + 1) * frame_sz_16]
        gray, lo, hi = raw16_to_gray(chunk)

        # Grayscale
        img_g = Image.frombytes("L", (W, H), gray)
        fname_g = outdir / f"raw16_gray_{ts}_{i:03d}.png"
        img_g.save(fname_g)

        # Iron-bow colorized
        rgb_c = colorize(gray, lut)
        img_c = Image.frombytes("RGB", (W, H), rgb_c)
        label16 = info16.strip().split("\n")[0] if info16.strip() else ""
        annotate(img_c, f"RAW16 #{i}  [{lo}-{hi}]\n{label16}")
        fname_c = outdir / f"raw16_iron_{ts}_{i:03d}.png"
        img_c.save(fname_c)
        print(f"  {fname_g}  {fname_c}")

    # --- Calibration stats ---
    if n_raw >= 2:
        n = W * H
        px0 = struct.unpack(f"<{n}H", raw16[:frame_sz_16])
        px1 = struct.unpack(f"<{n}H", raw16[frame_sz_16:2 * frame_sz_16])
        diffs = [abs(a - b) for a, b in zip(px0, px1)]
        mean_d = sum(diffs) / n
        max_d = max(diffs)
        var_d = sum((d - mean_d) ** 2 for d in diffs) / n
        std_d = var_d ** 0.5
        lo0, hi0 = min(px0), max(px0)
        print(f"\n[Calibration]")
        print(f"  RAW16 range: [{lo0} - {hi0}] ({hi0 - lo0} counts)")
        print(f"  Frame delta: mean={mean_d:.1f}  max={max_d}  std={std_d:.2f}")
        print(f"  Temporal noise (frame-to-frame std): {std_d:.2f} counts")

    print(f"\nDone: {n_agc} AGC8 + {n_raw} RAW16 PNGs in {outdir}/")


if __name__ == "__main__":
    main()
