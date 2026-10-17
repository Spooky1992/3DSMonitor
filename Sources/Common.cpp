#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "Capture.hpp"
#include "Common.hpp"

namespace CTRPluginFramework
{
    std::string g_recDir;

    void    Log(const char *fmt, ...)
    {
        char    line[256];
        int     len;
        time_t  now = time(nullptr);
        struct tm t;

        gmtime_r(&now, &t); ///< The console clock is local time, no timezone to apply
        len = snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                       t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

        va_list args;
        va_start(args, fmt);
        len += vsnprintf(line + len, sizeof(line) - len - 1, fmt, args);
        va_end(args);

        if (len > (int)sizeof(line) - 2)
            len = sizeof(line) - 2;
        line[len++] = '\n';

        File    file;
        if (File::Open(file, g_recDir + "/3dsmonitor.log", File::WRITE | File::CREATE | File::APPEND) == File::SUCCESS)
        {
            file.Write(line, len);
            file.Close();
        }
    }

    // Directory::Create is not recursive and returns SUCCESS if the folder
    // already exists.
    int     CreateRecDir(void)
    {
        std::string tid;
        Process::GetTitleID(tid);

        std::string path = "/luma/plugins/" + tid;
        g_recDir = path + "/rec";

        int res = Directory::Create(path);
        if (res == Directory::SUCCESS)
            res = Directory::Create(g_recDir);
        return res;
    }

    std::string TimestampName(const char *prefix)
    {
        time_t  now = time(nullptr);
        struct tm t;
        char    name[80];

        gmtime_r(&now, &t);
        snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d.raw", prefix,
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        return name;
    }

    void    InitHeader(RecHeader &hdr, u32 srcFormat, u32 fps, u32 frameCount, u32 droppedFrames)
    {
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, REC_MAGIC, 4);
        hdr.version = REC_VERSION;
        hdr.headerSize = sizeof(RecHeader);
        hdr.width = kWidth;
        hdr.height = kHeight;
        hdr.pixelFormat = REC_PIXEL_RGB565;
        hdr.layout = REC_LAYOUT_COLUMNS;
        hdr.srcFormat = srcFormat;
        hdr.fps = fps;
        hdr.frameCount = frameCount;
        hdr.droppedFrames = droppedFrames;
    }

    // Unless FwkSettings::CachedDrawMode is set, CTRPF does not hand us the
    // game's virtual address but PA_FROM_VA(addr) (OSDImpl.cpp GetBuffer()):
    // the physical address with bit 31 set (PA_RWX in csvc.h), a mapping of
    // physical memory that CTRPF itself draws into without any cache flush.
    // Seen on console: 0x98300000 / 0x98346600 = VRAM 0x18300000 / 0x18346600.
    // Process::CheckAddress() does not know that mapping, so check the
    // physical range instead.
    bool    IsReadableFramebuffer(u32 addr, u32 size, bool &isPhysical)
    {
        isPhysical = (addr & 0x80000000) != 0;
        if (isPhysical)
        {
            const u32 pa = addr & 0x7FFFFFFF;
            const u32 end = pa + size;
            const bool inVram = pa >= 0x18000000 && end <= 0x18600000;
            const bool inFcram = pa >= 0x20000000 && end <= 0x30000000; ///< PA_RWX only maps PAs below 0x30000000
            return inVram || inFcram;
        }
        return addr && Process::CheckAddress(addr, MEMPERM_READ)
                    && Process::CheckAddress(addr + size - 1, MEMPERM_READ);
    }

    bool    ConvertTopScreen(const Screen &screen, u16 *dst, u64 &ticks)
    {
        const u32 addr = screen.LeftFramebuffer; ///< Left eye when 3D is on
        const u32 size = screen.Stride * kWidth;
        bool isPhysical;

        ticks = 0;
        if (!IsReadableFramebuffer(addr, size, isPhysical))
            return false;

        const u64 start = svcGetSystemTick();

        // Virtual address (CachedDrawMode): the GPU wrote the framebuffer behind
        // the CPU cache's back, write back and drop the cached lines of that
        // range so we read the real frame. Flush (not invalidate) so we don't
        // drop pixels CTRPF's OSD just drew. The physical mapping is used by
        // CTRPF without any flush, so we don't flush it either.
        if (!isPhysical)
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, addr, size);

        const bool ok = Capture::ToRGB565(reinterpret_cast<const u8 *>(addr), screen.Stride,
                                          screen.Format, dst, kWidth, kHeight);
        ticks = svcGetSystemTick() - start;
        return ok;
    }
}
