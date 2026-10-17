#!/usr/bin/env python3
"""Converts a 3DSMonitor recording (.raw) to .mp4 with ffmpeg.

Usage:
    python convert.py rec_20261017_181500.raw          -> rec_20261017_181500.mp4
    python convert.py rec.raw -o video.mp4 --crf 23

ffmpeg is taken from the PATH, or from the imageio-ffmpeg Python package
(`python -m pip install imageio-ffmpeg`, which ships its own ffmpeg).
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rawfile  # noqa: E402

CHUNK_FRAMES = 16


def find_ffmpeg():
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return None


def ffmpeg_command(ffmpeg, hdr, fps, crf, output):
    if hdr.layout == rawfile.LAYOUT_COLUMNS:
        # Each 3DS column (screen X) is stored as one row of `height` pixels,
        # bottom of the screen first: rotating 90 degrees counterclockwise
        # gives the screen back (checked by tests/host/check_raw.py).
        size, vf = "%dx%d" % (hdr.height, hdr.width), ["-vf", "transpose=2"]
    else:
        size, vf = "%dx%d" % (hdr.width, hdr.height), []
    return ([ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
             "-f", "rawvideo", "-pixel_format", "rgb565le", "-video_size", size,
             "-framerate", str(fps), "-i", "-"]
            + vf
            + ["-c:v", "libx264", "-preset", "medium", "-crf", str(crf), "-pix_fmt", "yuv420p",
               "-movflags", "+faststart", output])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="rec_*.raw file written by the plugin")
    parser.add_argument("-o", "--output", help="output .mp4 (default: input name with .mp4)")
    parser.add_argument("--fps", type=int, help="override the frame rate stored in the file")
    parser.add_argument("--crf", type=int, default=18, help="x264 quality, lower = better (default: 18)")
    args = parser.parse_args()

    output = args.output or os.path.splitext(args.input)[0] + ".mp4"

    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        sys.exit("error: ffmpeg not found. Install it with:\n"
                 "    python -m pip install imageio-ffmpeg\n"
                 "(or put ffmpeg in your PATH)")

    try:
        f = open(args.input, "rb")
    except OSError as e:
        sys.exit("error: %s" % e)

    with f:
        try:
            hdr = rawfile.read_header(f)
        except rawfile.RawFormatError as e:
            sys.exit("error: %s" % e)

        frames = rawfile.frames_in_file(hdr, os.fstat(f.fileno()).st_size)
        fps = args.fps or hdr.fps
        print(hdr.describe())
        if hdr.frame_count == 0:
            print("note: frame count missing in header (recording interrupted), using %d frames found in the file"
                  % frames)
        if frames == 0:
            sys.exit("error: no frame in this file")
        if fps <= 0:
            sys.exit("error: no frame rate in the file (screenshot?), use --fps or raw2png.py")

        proc = subprocess.Popen(ffmpeg_command(ffmpeg, hdr, fps, args.crf, output), stdin=subprocess.PIPE)
        f.seek(hdr.header_size)
        remaining = frames
        try:
            while remaining:
                n = min(remaining, CHUNK_FRAMES)
                proc.stdin.write(f.read(n * hdr.frame_size))
                remaining -= n
            proc.stdin.close()
        except BrokenPipeError:
            pass
        if proc.wait() != 0:
            sys.exit("error: ffmpeg failed (code %d)" % proc.returncode)

    print("wrote %s (%d frames, %.1f s at %d fps)" % (output, frames, frames / float(fps), fps))


if __name__ == "__main__":
    main()
