#ifndef CAPTURE_HPP
#define CAPTURE_HPP

// Framebuffer -> RGB565 conversion.
//
// Header-only and free of any 3DS dependency so it can also be compiled and
// unit-tested on a PC (see tests/host).
//
// 3DS framebuffer layout (verified in CTRPluginFramework sources:
// OSDImpl.cpp walks `startAddr + stride * x` for x in [0, 400) with 240
// pixels per column; Screenshot.cpp treats the first pixel of a column as the
// bottom row of the screen):
//   - the screen is stored rotated: one "column" per screen X coordinate,
//   - a column holds 240 pixels, `stride` bytes apart from the next column,
//   - pixel i of column x is the screen pixel (x, 239 - i).
//
// The output keeps that column-major order (layout REC_LAYOUT_COLUMNS in
// RecFormat.hpp): reads and writes stay sequential, which is the cheapest
// order on the ARM11. The PC tools rotate the image back.

#include <stdint.h>

namespace Capture
{
    // Same values as libctru's GSPGPU_FramebufferFormat (gspgpu.h).
    enum SrcFormat : uint32_t
    {
        SRC_RGBA8  = 0, ///< 4 bytes, u32 0xRRGGBBAA (bytes in memory: A B G R)
        SRC_BGR8   = 1, ///< 3 bytes, bytes in memory: B G R
        SRC_RGB565 = 2, ///< 2 bytes
        SRC_RGB5A1 = 3, ///< 2 bytes, RRRRRGGGGGBBBBBA
        SRC_RGBA4  = 4, ///< 2 bytes, RRRRGGGGBBBBAAAA
    };

    static inline uint32_t BytesPerPixel(uint32_t format)
    {
        switch (format & 7)
        {
            case SRC_RGBA8: return 4;
            case SRC_BGR8:  return 3;
            case SRC_RGB565:
            case SRC_RGB5A1:
            case SRC_RGBA4: return 2;
            default:        return 0;
        }
    }

    /**
     * Converts `width` columns of `height` pixels from a framebuffer to
     * RGB565, keeping the column-major order.
     * \param src    First byte of the framebuffer (column 0, pixel 0)
     * \param stride Bytes between two columns
     * \param format Source format (SrcFormat)
     * \param dst    Output, width * height u16
     * \return false if the format is unknown or the stride too small
     */
    static inline bool ToRGB565(const uint8_t *src, uint32_t stride, uint32_t format,
                                uint16_t *dst, uint32_t width, uint32_t height)
    {
        format &= 7;
        const uint32_t bpp = BytesPerPixel(format);

        if (bpp == 0 || stride < bpp * height)
            return false;

        for (uint32_t x = 0; x < width; ++x, src += stride)
        {
            switch (format)
            {
                case SRC_RGBA8:
                {
                    const uint32_t *p = reinterpret_cast<const uint32_t *>(src);
                    for (uint32_t i = 0; i < height; ++i)
                    {
                        const uint32_t c = p[i];
                        *dst++ = ((c >> 16) & 0xF800) | ((c >> 13) & 0x07E0) | ((c >> 11) & 0x001F);
                    }
                    break;
                }
                case SRC_BGR8:
                {
                    // The framebuffer may be read through an uncached mapping:
                    // read 3 words for 4 pixels instead of 12 single bytes.
                    if ((reinterpret_cast<uintptr_t>(src) & 3) == 0 && (height & 3) == 0)
                    {
                        const uint32_t *p = reinterpret_cast<const uint32_t *>(src);
                        for (uint32_t i = 0; i < height; i += 4, p += 3)
                        {
                            // Little endian: w0 = B0 G0 R0 B1, w1 = G1 R1 B2 G2, w2 = R2 B3 G3 R3
                            const uint32_t w0 = p[0], w1 = p[1], w2 = p[2];
                            *dst++ = ((w0 >> 8) & 0xF800) | ((w0 >> 5) & 0x07E0) | ((w0 >> 3) & 0x001F);
                            *dst++ = (w1 & 0xF800) | ((w1 << 3) & 0x07E0) | (w0 >> 27);
                            *dst++ = ((w2 << 8) & 0xF800) | ((w1 >> 21) & 0x07E0) | ((w1 >> 19) & 0x001F);
                            *dst++ = (w2 & 0xF8000000) >> 16 | ((w2 >> 13) & 0x07E0) | ((w2 >> 11) & 0x001F);
                        }
                    }
                    else
                    {
                        const uint8_t *p = src;
                        for (uint32_t i = 0; i < height; ++i, p += 3)
                            *dst++ = ((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3);
                    }
                    break;
                }
                case SRC_RGB565:
                {
                    const uint16_t *p = reinterpret_cast<const uint16_t *>(src);
                    for (uint32_t i = 0; i < height; ++i)
                        *dst++ = p[i];
                    break;
                }
                case SRC_RGB5A1:
                {
                    const uint16_t *p = reinterpret_cast<const uint16_t *>(src);
                    for (uint32_t i = 0; i < height; ++i)
                    {
                        const uint16_t c = p[i];
                        *dst++ = (c & 0xFFC0) | ((c >> 1) & 0x001F);
                    }
                    break;
                }
                case SRC_RGBA4:
                {
                    const uint16_t *p = reinterpret_cast<const uint16_t *>(src);
                    for (uint32_t i = 0; i < height; ++i)
                    {
                        const uint16_t c = p[i];
                        *dst++ = ((c & 0xF000)) | ((c & 0x0F00) >> 1) | ((c & 0x00F0) >> 3);
                    }
                    break;
                }
            }
        }
        return true;
    }
}

#endif
