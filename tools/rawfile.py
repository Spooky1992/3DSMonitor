"""Reader for the .raw files written by the 3DSMonitor plugin.

Format (keep in sync with Includes/RecFormat.hpp): a 32-byte little-endian
header followed by `frame_count` frames of width * height pixels.
"""

import struct

MAGIC = b"3DSR"
HEADER_STRUCT = struct.Struct("<4sHHHHBBBBHHIII")
assert HEADER_STRUCT.size == 32

PIXEL_RGB565 = 0

LAYOUT_ROWS = 0     # rows top to bottom, pixels left to right
LAYOUT_COLUMNS = 1  # 3DS framebuffer order: for x, for i: pixel (x, height - 1 - i)

SRC_FORMAT_NAMES = {0: "RGBA8", 1: "BGR8", 2: "RGB565", 3: "RGB5A1", 4: "RGBA4"}


class RawFormatError(Exception):
    pass


class RawHeader:
    def __init__(self, data):
        if len(data) < HEADER_STRUCT.size:
            raise RawFormatError("file too small for a header")
        (self.magic, self.version, self.header_size, self.width, self.height,
         self.pixel_format, self.layout, self.src_format, self.flags, self.fps,
         _reserved0, self.frame_count, self.dropped_frames,
         _reserved1) = HEADER_STRUCT.unpack_from(data)
        if self.magic != MAGIC:
            raise RawFormatError("bad magic %r (expected %r)" % (self.magic, MAGIC))
        if self.version != 1:
            raise RawFormatError("unsupported version %d" % self.version)
        if self.pixel_format != PIXEL_RGB565:
            raise RawFormatError("unsupported pixel format %d" % self.pixel_format)
        if self.layout not in (LAYOUT_ROWS, LAYOUT_COLUMNS):
            raise RawFormatError("unsupported layout %d" % self.layout)

    @property
    def frame_size(self):
        return self.width * self.height * 2

    def describe(self):
        return ("%dx%d RGB565, layout=%s, game format=%s, fps=%d, frames=%d, dropped=%d" % (
            self.width, self.height,
            "columns" if self.layout == LAYOUT_COLUMNS else "rows",
            SRC_FORMAT_NAMES.get(self.src_format, str(self.src_format)),
            self.fps, self.frame_count, self.dropped_frames))


def frames_in_file(hdr, file_size):
    """Number of complete frames to use.

    frameCount is 0 when the game exited during a recording (the plugin
    could not rewrite the header): count the complete frames in the file.
    """
    available = max(0, (file_size - hdr.header_size) // hdr.frame_size)
    if hdr.frame_count == 0 or hdr.frame_count > available:
        return available
    return hdr.frame_count


def read_header(f):
    hdr = RawHeader(f.read(HEADER_STRUCT.size))
    f.seek(hdr.header_size)
    return hdr


def read_frame(f, hdr, index):
    """Returns the raw RGB565 bytes of frame `index`."""
    f.seek(hdr.header_size + index * hdr.frame_size)
    data = f.read(hdr.frame_size)
    if len(data) != hdr.frame_size:
        raise RawFormatError("frame %d is truncated" % index)
    return data


def _rgb565_lut():
    lut = []
    for c in range(65536):
        r5, g6, b5 = c >> 11, (c >> 5) & 0x3F, c & 0x1F
        # Bit replication so that 0x1F -> 0xFF and 0 -> 0
        lut.append(bytes(((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))))
    return lut


_LUT = None


def frame_to_rgb_rows(hdr, data):
    """Converts one frame to a list of RGB888 rows (top to bottom)."""
    global _LUT
    if _LUT is None:
        _LUT = _rgb565_lut()
    lut = _LUT
    w, h = hdr.width, hdr.height
    px = struct.unpack("<%dH" % (w * h), data)

    if hdr.layout == LAYOUT_ROWS:
        return [b"".join(lut[c] for c in px[y * w:(y + 1) * w]) for y in range(h)]

    # LAYOUT_COLUMNS: screen pixel (x, y) is px[x * h + (h - 1 - y)]
    return [b"".join(lut[px[x * h + (h - 1 - y)]] for x in range(w)) for y in range(h)]
