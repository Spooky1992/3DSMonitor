#!/usr/bin/env python3
"""Converts a 3DSMonitor .raw file (screenshot or recording) to PNG.

Usage:
    python raw2png.py shot_20261017_181500.raw            -> shot_20261017_181500.png
    python raw2png.py rec.raw --frame 10 -o frame10.png    -> one frame of a recording

Only uses the Python standard library (3.6+).
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rawfile  # noqa: E402


def write_png(path, width, height, rows):
    """Writes RGB888 rows as an 8-bit RGB PNG."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)  # filter type 0 on each row
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help=".raw file written by the plugin")
    parser.add_argument("-o", "--output", help="output .png (default: input name with .png)")
    parser.add_argument("--frame", type=int, default=0, help="frame index (default: 0)")
    args = parser.parse_args()

    output = args.output or os.path.splitext(args.input)[0] + ".png"

    try:
        with open(args.input, "rb") as f:
            hdr = rawfile.read_header(f)
            print(hdr.describe())
            frames = rawfile.frames_in_file(hdr, os.fstat(f.fileno()).st_size)
            if not 0 <= args.frame < frames:
                sys.exit("error: frame %d out of range (file has %d frames)" % (args.frame, frames))
            data = rawfile.read_frame(f, hdr, args.frame)
    except (OSError, rawfile.RawFormatError) as e:
        sys.exit("error: %s" % e)

    write_png(output, hdr.width, hdr.height, rawfile.frame_to_rgb_rows(hdr, data))
    print("wrote %s" % output)


if __name__ == "__main__":
    main()
