// Host (PC) unit test for Includes/Capture.hpp.
//
// Builds fake 3DS framebuffers in every GSP format, converts them with
// Capture::ToRGB565 and compares against a reference decoder that copies the
// formulas of CTRPluginFramework's Screenshot.cpp (ScreenToBMP_*), truncated
// to RGB565. Also writes out/<format>.raw files for tests/host/check_raw.py.
//
// Build & run: tests/host/run.sh

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "Capture.hpp"
#include "RecFormat.hpp"

static const uint32_t W = 400, H = 240;

// Test pattern, screen coordinates (origin top-left). Must match check_raw.py.
static void Pattern(uint32_t x, uint32_t y, uint8_t &r, uint8_t &g, uint8_t &b)
{
    r = (uint8_t)(x * 255 / (W - 1));
    g = (uint8_t)(y * 255 / (H - 1));
    b = (uint8_t)((x * 3 + y * 5) & 0xFF);
}

static void Encode(uint32_t format, uint8_t *p, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t c;
    switch (format)
    {
        case Capture::SRC_RGBA8:
            c = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
            memcpy(p, &c, 4);
            break;
        case Capture::SRC_BGR8:
            p[0] = b; p[1] = g; p[2] = r;
            break;
        case Capture::SRC_RGB565:
            c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            p[0] = c & 0xFF; p[1] = c >> 8;
            break;
        case Capture::SRC_RGB5A1:
            c = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
            p[0] = c & 0xFF; p[1] = c >> 8;
            break;
        case Capture::SRC_RGBA4:
            c = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | 0xF;
            p[0] = c & 0xFF; p[1] = c >> 8;
            break;
    }
}

// Reference: CTRPF Screenshot.cpp ScreenToBMP_* formulas -> 8-bit channels.
static uint16_t Reference(uint32_t format, const uint8_t *p)
{
    uint8_t r = 0, g = 0, b = 0;
    uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
    switch (format)
    {
        case Capture::SRC_RGBA8:  b = p[1]; g = p[2]; r = p[3]; break; // skip alpha byte
        case Capture::SRC_BGR8:   b = p[0]; g = p[1]; r = p[2]; break;
        case Capture::SRC_RGB565: b = (c << 3) & 0xF8; g = (c >> 3) & 0xFC; r = (c >> 8) & 0xF8; break;
        case Capture::SRC_RGB5A1: b = (c << 2) & 0xF8; g = (c >> 3) & 0xF8; r = (c >> 8) & 0xF8; break;
        case Capture::SRC_RGBA4:  b = c & 0xF0; g = (c >> 4) & 0xF0; r = (c >> 8) & 0xF0; break;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int TestFormat(uint32_t format, const char *name)
{
    const uint32_t bpp = Capture::BytesPerPixel(format);
    const uint32_t stride = bpp * H + 16; // padding between columns to test stride handling
    std::vector<uint8_t> fb(stride * W + 4, 0xCD);
    std::vector<uint16_t> out(W * H);

    // Fill like the 3DS: column x, pixel i = screen (x, H - 1 - i)
    for (uint32_t x = 0; x < W; ++x)
        for (uint32_t i = 0; i < H; ++i)
        {
            uint8_t r, g, b;
            Pattern(x, H - 1 - i, r, g, b);
            Encode(format, &fb[x * stride + i * bpp], r, g, b);
        }

    if (!Capture::ToRGB565(fb.data(), stride, format, out.data(), W, H))
    {
        printf("FAIL %s: ToRGB565 returned false\n", name);
        return 1;
    }

    int errors = 0;
    for (uint32_t x = 0; x < W; ++x)
        for (uint32_t i = 0; i < H; ++i)
        {
            uint16_t expected = Reference(format, &fb[x * stride + i * bpp]);
            uint16_t got = out[x * H + i];
            if (got != expected && errors++ < 5)
                printf("FAIL %s: column %u pixel %u: got %04X expected %04X\n", name, x, i, got, expected);
        }

    // Write a .raw like the plugin does
    RecHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, REC_MAGIC, 4);
    hdr.version = REC_VERSION;
    hdr.headerSize = sizeof(RecHeader);
    hdr.width = W;
    hdr.height = H;
    hdr.pixelFormat = REC_PIXEL_RGB565;
    hdr.layout = REC_LAYOUT_COLUMNS;
    hdr.srcFormat = (uint8_t)format;
    hdr.frameCount = 1;

    char path[256];
    snprintf(path, sizeof(path), "tests/host/out/%s.raw", name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(&hdr, sizeof(hdr), 1, f) != 1 || fwrite(out.data(), 2, out.size(), f) != out.size())
    {
        printf("FAIL %s: can't write %s\n", name, path);
        errors++;
    }
    if (f)
        fclose(f);

    printf("%s %s (%d mismatches)\n", errors ? "FAIL" : "ok  ", name, errors);
    return errors ? 1 : 0;
}

int main()
{
    int failed = 0;
    failed += TestFormat(Capture::SRC_RGBA8, "rgba8");
    failed += TestFormat(Capture::SRC_BGR8, "bgr8");
    failed += TestFormat(Capture::SRC_RGB565, "rgb565");
    failed += TestFormat(Capture::SRC_RGB5A1, "rgb5a1");
    failed += TestFormat(Capture::SRC_RGBA4, "rgba4");

    // Invalid inputs must be rejected
    uint8_t dummy[4] = {0};
    uint16_t o;
    if (Capture::ToRGB565(dummy, 4, 7, &o, 1, 1)) { printf("FAIL unknown format accepted\n"); failed++; }
    if (Capture::ToRGB565(dummy, 2, Capture::SRC_RGBA8, &o, 1, 1)) { printf("FAIL small stride accepted\n"); failed++; }

    printf(failed ? "FAILED\n" : "ALL OK\n");
    return failed ? 1 : 0;
}
