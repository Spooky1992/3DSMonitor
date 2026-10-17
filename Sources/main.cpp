#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "Capture.hpp"
#include "RecFormat.hpp"

namespace CTRPluginFramework
{
    // Phase 1: L+R+START takes one screenshot of the top screen (left eye)
    // and writes it as a .raw file (RecHeader + 1 frame of RGB565).
    //
    // Threads:
    //  - the OSD callback runs in the game's thread, inside CTRPF's hook of
    //    the framebuffer swap (OSDImpl.cpp). It must stay short: it only
    //    converts the framebuffer into a buffer allocated at startup.
    //  - the menu callback runs in the plugin's thread (PluginMenuImpl main
    //    loop, right after Controller::Update()). It reads the keys and does
    //    the slow SD card writes.
    // They talk through g_shotState (atomic accesses).

    static const u32    kHotkey = Key::L | Key::R | Key::Start;
    static const u32    kWidth = 400;
    static const u32    kHeight = 240;
    static const u32    kFrameBytes = kWidth * kHeight * sizeof(u16);

    enum : s32
    {
        SHOT_IDLE = 0,      ///< Waiting for the hotkey (plugin thread owns everything)
        SHOT_REQUESTED,     ///< Hotkey pressed, OSD callback must capture the next top frame
        SHOT_DONE,          ///< Buffer filled, plugin thread must write it
        SHOT_FAILED,        ///< Capture impossible, plugin thread must report it
    };

    static s32          g_shotState = SHOT_IDLE;
    static u16 *        g_shotBuffer = nullptr;
    // Written by the OSD callback before it publishes SHOT_DONE / SHOT_FAILED.
    static u32          g_shotAddr = 0;
    static u32          g_shotStride = 0;
    static u32          g_shotFormat = 0;
    static u64          g_shotTicks = 0;

    static std::string  g_recDir;   ///< /luma/plugins/<TitleID>/rec

    static inline s32   LoadState(void)
    {
        return __atomic_load_n(&g_shotState, __ATOMIC_ACQUIRE);
    }

    static inline void  StoreState(s32 state)
    {
        __atomic_store_n(&g_shotState, state, __ATOMIC_RELEASE);
    }

    static inline float TicksToMs(u64 ticks)
    {
        return (float)ticks * 1000.f / (float)SYSCLOCK_ARM11;
    }

    // Appends one line to <recDir>/3dsmonitor.log. Plugin thread only.
    static void     Log(const char *fmt, ...)
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

    // Creates /luma/plugins/<TitleID>/rec. Directory::Create is not
    // recursive and returns SUCCESS if the folder already exists.
    static int      CreateRecDir(void)
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

    // Game thread: called by CTRPF for each screen, every frame.
    static bool     CaptureCallback(const Screen &screen)
    {
        if (!screen.IsTop || LoadState() != SHOT_REQUESTED)
            return false;

        const u32 addr = screen.LeftFramebuffer; ///< Left eye when 3D is on
        const u32 size = screen.Stride * kWidth;

        g_shotAddr = addr;
        g_shotStride = screen.Stride;
        g_shotFormat = screen.Format;

        if (!addr || !Process::CheckAddress(addr, MEMPERM_READ)
                  || !Process::CheckAddress(addr + size - 1, MEMPERM_READ))
        {
            StoreState(SHOT_FAILED);
            return false;
        }

        const u64 start = svcGetSystemTick();

        // The GPU wrote the framebuffer behind the CPU cache's back: write back
        // and drop any cached line of that range so we read the real frame.
        // Flush (not invalidate) so we don't drop pixels CTRPF's OSD just drew.
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, addr, size);

        const bool ok = Capture::ToRGB565(reinterpret_cast<const u8 *>(addr), screen.Stride,
                                          screen.Format, g_shotBuffer, kWidth, kHeight);

        g_shotTicks = svcGetSystemTick() - start;
        StoreState(ok ? SHOT_DONE : SHOT_FAILED);
        return false;
    }

    // Plugin thread: writes the captured frame to the SD card.
    static void     SaveShot(void)
    {
        time_t  now = time(nullptr);
        struct tm t;
        char    name[80];

        gmtime_r(&now, &t);
        snprintf(name, sizeof(name), "shot_%04d%02d%02d_%02d%02d%02d.raw",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

        RecHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, REC_MAGIC, 4);
        hdr.version = REC_VERSION;
        hdr.headerSize = sizeof(RecHeader);
        hdr.width = kWidth;
        hdr.height = kHeight;
        hdr.pixelFormat = REC_PIXEL_RGB565;
        hdr.layout = REC_LAYOUT_COLUMNS;
        hdr.srcFormat = g_shotFormat;
        hdr.fps = 0;
        hdr.frameCount = 1;

        const u64 start = svcGetSystemTick();
        File    file;
        int     res = File::Open(file, g_recDir + "/" + name, File::WRITE | File::CREATE | File::TRUNCATE);

        if (res == File::SUCCESS)
        {
            res = file.Write(&hdr, sizeof(hdr));
            if (res == File::SUCCESS)
                res = file.Write(g_shotBuffer, kFrameBytes);
            int closeRes = file.Close();
            if (res == File::SUCCESS)
                res = closeRes;
        }

        const float writeMs = TicksToMs(svcGetSystemTick() - start);

        Log("shot %s res=%d fmt=%lu stride=%lu addr=%08lX convert=%.2fms write=%.2fms",
            name, res, g_shotFormat, g_shotStride, g_shotAddr, TicksToMs(g_shotTicks), writeMs);

        if (res == File::SUCCESS)
            OSD::Notify(std::string("Saved ") + name);
        else
            OSD::Notify(Utils::Format("Screenshot write error %08X", res), Color::Red);
    }

    // Plugin thread: called on every iteration of the plugin's main loop.
    static void     Poll(void)
    {
        switch (LoadState())
        {
            case SHOT_IDLE:
                if (g_shotBuffer && Controller::IsKeysPressed(kHotkey))
                    StoreState(SHOT_REQUESTED);
                break;

            case SHOT_DONE:
                SaveShot();
                StoreState(SHOT_IDLE);
                break;

            case SHOT_FAILED:
                Log("shot failed fmt=%lu stride=%lu addr=%08lX", g_shotFormat, g_shotStride, g_shotAddr);
                OSD::Notify("Screenshot failed (see log)", Color::Red);
                StoreState(SHOT_IDLE);
                break;

            default: ///< SHOT_REQUESTED: waiting for the game's next frame
                break;
        }
    }

    // Called before main and before the game starts.
    void    PatchProcess(FwkSettings &settings)
    {
        // We don't need the Action Replay engine nor the memory searcher:
        // disabling them keeps the plugin's memory footprint small.
        settings.AllowActionReplay = false;
        settings.AllowSearchEngine = false;
    }

    // Called when the process exits.
    void    OnProcessExit(void)
    {
    }

    int     main(void)
    {
        PluginMenu *menu = new PluginMenu("3DSMonitor", 0, 1, 0,
                                          "Gameplay recorder for Old 3DS.\n"
                                          "Phase 1: L+R+START saves a screenshot of the top screen\n"
                                          "to /luma/plugins/<TitleID>/rec/.");

        menu->SynchronizeWithFrame(true);

        *menu += new MenuEntry("Screenshot: L+R+START", nullptr, [](MenuEntry *entry)
        {
            MessageBox("3DSMonitor", "Press L+R+START in game.\nFiles go to:\n" + g_recDir)();
        });

        int dirRes = CreateRecDir();

        // Allocated once: nothing is allocated in the per-frame callback.
        g_shotBuffer = static_cast<u16 *>(malloc(kFrameBytes));

        Log("3DSMonitor 0.1.0 started, dir=%s dirRes=%d buffer=%s",
            g_recDir.c_str(), dirRes, g_shotBuffer ? "ok" : "ALLOC FAILED");

        if (dirRes != Directory::SUCCESS)
            OSD::Notify(Utils::Format("3DSMonitor: can't create rec dir (%08X)", dirRes), Color::Red);
        else if (!g_shotBuffer)
            OSD::Notify("3DSMonitor: out of memory", Color::Red);
        else
            OSD::Notify("3DSMonitor ready: L+R+START = screenshot");

        OSD::Run(CaptureCallback);
        *menu += Poll;

        menu->Run();

        OSD::Stop(CaptureCallback);
        delete menu;
        free(g_shotBuffer);
        g_shotBuffer = nullptr;
        return 0;
    }
}
