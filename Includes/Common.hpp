#ifndef COMMON_HPP
#define COMMON_HPP

#include <3ds.h>
#include <string>

#include "RecFormat.hpp"

namespace CTRPluginFramework
{
    static const u32    kWidth = 400;
    static const u32    kHeight = 240;
    static const u32    kFrameBytes = kWidth * kHeight * sizeof(u16);

    extern std::string  g_recDir;   ///< /luma/plugins/<TitleID>/rec

    static inline float TicksToMs(u64 ticks)
    {
        return (float)ticks * 1000.f / (float)SYSCLOCK_ARM11;
    }

    /// Appends one line to <recDir>/3dsmonitor.log. Plugin thread only.
    void        Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    /// Creates /luma/plugins/<TitleID>/rec and sets g_recDir.
    int         CreateRecDir(void);

    /// "<prefix>_YYYYMMDD_HHMMSS.raw" from the console clock.
    std::string TimestampName(const char *prefix);

    /// Fills a RecHeader for a top screen capture (RGB565, column layout).
    void        InitHeader(RecHeader &hdr, u32 srcFormat, u32 fps, u32 frameCount, u32 droppedFrames);

    /// Is [addr, addr + size) a framebuffer we can read from this process?
    /// isPhysical tells whether addr is CTRPF's physical mapping (no cache flush needed).
    bool        IsReadableFramebuffer(u32 addr, u32 size, bool &isPhysical);

    class Screen;

    /// Game thread. Converts the top screen's left framebuffer to RGB565
    /// (column layout) into dst (kFrameBytes). ticks = time spent.
    /// False if the framebuffer is not readable or its format unknown.
    bool        ConvertTopScreen(const Screen &screen, u16 *dst, u64 &ticks);
}

#endif
