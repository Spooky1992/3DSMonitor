#!/usr/bin/env python3
"""Checks the .raw files written by test_capture.cpp with the PC tools.

Verifies that tools/rawfile.py puts every pixel back at its screen position
(orientation) with the expected colour, and that tools/raw2png.py writes a
PNG file.
"""

import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rawfile  # noqa: E402

W, H = 400, 240
OUT = os.path.join(ROOT, "tests", "host", "out")


def pattern(x, y):  # must match Pattern() in test_capture.cpp
    return x * 255 // (W - 1), y * 255 // (H - 1), (x * 3 + y * 5) & 0xFF


def expand(v, bits):
    v >>= 8 - bits
    return (v << (8 - bits)) | (v >> (2 * bits - 8))


def check_rgba8_orientation():
    with open(os.path.join(OUT, "rgba8.raw"), "rb") as f:
        hdr = rawfile.read_header(f)
        rows = rawfile.frame_to_rgb_rows(hdr, rawfile.read_frame(f, hdr, 0))
    assert (hdr.width, hdr.height, hdr.frame_count) == (W, H, 1), hdr.describe()
    assert len(rows) == H and all(len(r) == W * 3 for r in rows)
    errors = 0
    for y in range(H):
        for x in range(W):
            r, g, b = pattern(x, y)
            expected = bytes((expand(r, 5), expand(g, 6), expand(b, 5)))
            got = rows[y][x * 3:x * 3 + 3]
            if got != expected:
                errors += 1
                if errors <= 5:
                    print("FAIL pixel (%d,%d): got %s expected %s" % (x, y, got.hex(), expected.hex()))
    print("%s rgba8 orientation/colours (%d mismatches)" % ("FAIL" if errors else "ok  ", errors))
    return errors == 0


def check_png(name):
    raw = os.path.join(OUT, name + ".raw")
    png = os.path.join(OUT, name + ".png")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "raw2png.py"), raw, "-o", png],
                          stdout=subprocess.DEVNULL)
    with open(png, "rb") as f:
        data = f.read()
    ok = data[:8] == b"\x89PNG\r\n\x1a\n"
    # Decode IHDR + IDAT and check size and first pixel
    pos, idat, size = 8, b"", None
    while ok and pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            size = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    pixels = zlib.decompress(idat)
    ok = ok and size == (W, H) and len(pixels) == H * (1 + W * 3)
    print("%s %s.png" % ("ok  " if ok else "FAIL", name))
    return ok


def check_mp4():
    """Recording -> convert.py -> mp4, decoded back with ffmpeg: orientation and colours."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import convert  # noqa: E402
    ffmpeg = convert.find_ffmpeg()
    if not ffmpeg:
        if os.environ.get("REQUIRE_FFMPEG"):
            print("FAIL mp4: ffmpeg not found")
            return False
        print("skip mp4 (ffmpeg not found)")
        return True

    # 3 frames, header as left by a recording interrupted by the game exiting
    with open(os.path.join(OUT, "rgba8.raw"), "rb") as f:
        hdr_bytes, frame = f.read(32), f.read()
    hdr = bytearray(hdr_bytes)
    struct.pack_into("<H", hdr, 16, 15)  # fps
    struct.pack_into("<I", hdr, 20, 0)   # frameCount unknown
    raw = os.path.join(OUT, "rec_test.raw")
    mp4 = os.path.join(OUT, "rec_test.mp4")
    with open(raw, "wb") as f:
        f.write(bytes(hdr) + frame * 3 + frame[:1000])  # + a truncated frame to ignore
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "convert.py"), raw, "-o", mp4],
                          stdout=subprocess.DEVNULL)

    probe = subprocess.check_output([ffmpeg, "-v", "error", "-i", mp4, "-f", "rawvideo",
                                     "-pix_fmt", "rgb24", "-"])
    ok = len(probe) == 3 * W * H * 3
    diff = 0
    if ok:
        for y in range(0, H, 4):
            for x in range(0, W, 4):
                expected = [expand(c, 5 if i != 1 else 6) for i, c in enumerate(pattern(x, y))]
                got = probe[(y * W + x) * 3:(y * W + x) * 3 + 3]
                diff += sum(abs(a - b) for a, b in zip(got, expected))
        diff /= (W // 4) * (H // 4) * 3.0
        ok = diff < 12  # lossy H.264 + yuv420p; a wrong rotation gives ~80
    print("%s rec_test.mp4 (3 frames, mean abs diff %.1f)" % ("ok  " if ok else "FAIL", diff))
    return ok


def main():
    ok = check_rgba8_orientation()
    for name in ("rgba8", "bgr8", "rgb565", "rgb5a1", "rgba4"):
        ok = check_png(name) and ok
    ok = check_mp4() and ok
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
