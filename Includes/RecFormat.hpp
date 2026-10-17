#ifndef RECFORMAT_HPP
#define RECFORMAT_HPP

// On-disk format of the .raw files written by the plugin.
// Keep in sync with tools/rawfile.py.
//
// File = RecHeader (32 bytes, little endian) followed by `frameCount` frames
// of width * height pixels each.

#include <stdint.h>

#define REC_MAGIC           "3DSR"
#define REC_VERSION         1

#define REC_PIXEL_RGB565    0   ///< u16 little endian

#define REC_LAYOUT_ROWS     0   ///< Usual image order: rows top to bottom, pixels left to right
#define REC_LAYOUT_COLUMNS  1   ///< 3DS framebuffer order: for x in [0, width), for i in [0, height): pixel (x, height - 1 - i)

struct RecHeader
{
    char        magic[4];       ///< "3DSR"
    uint16_t    version;        ///< REC_VERSION
    uint16_t    headerSize;     ///< sizeof(RecHeader), frames start right after
    uint16_t    width;          ///< Image width (screen X)
    uint16_t    height;         ///< Image height (screen Y)
    uint8_t     pixelFormat;    ///< REC_PIXEL_*
    uint8_t     layout;         ///< REC_LAYOUT_*
    uint8_t     srcFormat;      ///< Framebuffer format of the game (GSPGPU_FramebufferFormat), for debugging
    uint8_t     flags;          ///< Reserved, 0
    uint16_t    fps;            ///< Nominal frames per second, 0 for a single screenshot
    uint16_t    reserved0;
    uint32_t    frameCount;     ///< Number of frames in the file
    uint32_t    droppedFrames;  ///< Frames lost while recording
    uint32_t    reserved1;
};

static_assert(sizeof(RecHeader) == 32, "RecHeader must be 32 bytes");

#endif
