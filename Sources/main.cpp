#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstring>
#include <string>

#include "Common.hpp"
#include "Recorder.hpp"

namespace CTRPluginFramework
{
    // L+R+DOWN starts / stops recording the top screen (Recorder.cpp).
    // L+R+UP saves one screenshot (when not recording).
    //
    // Threads:
    //  - the OSD callback runs in the game's thread, inside CTRPF's hook of
    //    the framebuffer swap (OSDImpl.cpp). It must stay short: it only
    //    converts the framebuffer into buffers allocated at startup.
    //  - the menu callback runs in the plugin's thread (PluginMenuImpl main
    //    loop, right after Controller::Update()). It reads the keys and does
    //    the slow SD card work (plus the recorder's own writer thread).
    // Screenshots talk through g_shotState (atomic accesses).

    // Not L+R+START: games use START combos for soft reset (the game restarts,
    // see the crash note at the end of main()).
    static const u32    kRecHotkey = Key::L | Key::R | Key::DPadDown;
    static const u32    kShotHotkey = Key::L | Key::R | Key::DPadUp;

    enum : s32
    {
        SHOT_IDLE = 0,      ///< Waiting for the hotkey (plugin thread owns everything)
        SHOT_REQUESTED,     ///< Hotkey pressed, OSD callback must capture the next top frame
        SHOT_DONE,          ///< Buffer filled, plugin thread must write it
        SHOT_FAILED,        ///< Capture impossible, plugin thread must report it
    };

    static s32          g_shotState = SHOT_IDLE;
    static u16 *        g_shotBuffer = nullptr; ///< Recorder's idle slot, only valid while not recording
    // Written by the OSD callback before it publishes SHOT_DONE / SHOT_FAILED.
    static u32          g_shotAddr = 0;
    static u32          g_shotStride = 0;
    static u32          g_shotFormat = 0;
    static u64          g_shotTicks = 0;

    static inline s32   LoadState(void)
    {
        return __atomic_load_n(&g_shotState, __ATOMIC_ACQUIRE);
    }

    static inline void  StoreState(s32 state)
    {
        __atomic_store_n(&g_shotState, state, __ATOMIC_RELEASE);
    }

    // Game thread: called by CTRPF for each screen, every frame.
    static bool     CaptureCallback(const Screen &screen)
    {
        if (!screen.IsTop)
            return false;

        Recorder::OnFrame(screen);

        if (LoadState() != SHOT_REQUESTED)
            return false;

        g_shotAddr = screen.LeftFramebuffer;
        g_shotStride = screen.Stride;
        g_shotFormat = screen.Format;

        const bool ok = ConvertTopScreen(screen, g_shotBuffer, g_shotTicks);
        StoreState(ok ? SHOT_DONE : SHOT_FAILED);
        return false;
    }

    // Plugin thread: writes the captured frame to the SD card.
    static void     SaveShot(void)
    {
        const std::string name = TimestampName("shot");

        RecHeader hdr;
        InitHeader(hdr, g_shotFormat, 0, 1, 0);

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
            name.c_str(), res, g_shotFormat, g_shotStride, g_shotAddr, TicksToMs(g_shotTicks), writeMs);

        if (res == File::SUCCESS)
            OSD::Notify("Saved " + name);
        else
            OSD::Notify(Utils::Format("Screenshot write error %08X", res), Color::Red);
    }

    static void     ToggleRecording(void)
    {
        if (Recorder::IsRecording())
        {
            Recorder::Stop();
            OSD::Notify("REC stopping...");
            return;
        }

        int res = Recorder::Start();
        if (res == 0)
            OSD::Notify("REC started (L+R+DOWN to stop)", Color::White, Color::Red);
        else
        {
            Log("rec start failed res=%d", res);
            OSD::Notify(Utils::Format("REC start error %08X", res), Color::Red);
        }
    }

    // Plugin thread: called on every iteration of the plugin's main loop.
    static void     Poll(void)
    {
        Recorder::Poll();

        switch (LoadState())
        {
            case SHOT_IDLE:
                if (Controller::IsKeysPressed(kRecHotkey))
                    ToggleRecording();
                else if (Controller::IsKeysPressed(kShotHotkey) && !Recorder::IsRecording())
                {
                    g_shotBuffer = Recorder::IdleBuffer();
                    if (g_shotBuffer)
                        StoreState(SHOT_REQUESTED);
                }
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
        PluginMenu *menu = new PluginMenu("3DSMonitor", 0, 2, 0,
                                          "Gameplay recorder for Old 3DS.\n"
                                          "L+R+DOWN: start / stop recording the top screen\n"
                                          "L+R+UP: screenshot\n"
                                          "Files: /luma/plugins/<TitleID>/rec/");

        menu->SynchronizeWithFrame(true);

        *menu += new MenuEntry("Help", nullptr, [](MenuEntry *entry)
        {
            MessageBox("3DSMonitor",
                       "L+R+DOWN: start / stop recording\nL+R+UP: screenshot\nFiles go to:\n" + g_recDir)();
        });

        int dirRes = CreateRecDir();

        // Allocated once: nothing is allocated in the per-frame callback.
        const bool ringOk = Recorder::Init();

        Log("3DSMonitor 0.2.0 started, dir=%s dirRes=%d ring=%lu slots",
            g_recDir.c_str(), dirRes, Recorder::SlotCount());

        if (dirRes != Directory::SUCCESS)
            OSD::Notify(Utils::Format("3DSMonitor: can't create rec dir (%08X)", dirRes), Color::Red);
        else if (!ringOk)
            OSD::Notify("3DSMonitor: out of memory", Color::Red);
        else
            OSD::Notify("3DSMonitor ready: L+R+DOWN = REC, L+R+UP = screenshot");

        OSD::Run(CaptureCallback);
        *menu += Poll;

        menu->Run();

        // Run() only returns when the game process exits. Deliberately no
        // `delete menu` / free(): on console (CTRPF 0.8.0, Luma 13.3.2) deleting
        // the menu at that point crashed in ~MenuFolderImpl -> ~MenuItem ->
        // free() on an already freed item (data abort). The process is going
        // away and Luma reclaims the whole plugin memory block anyway.
        // A recording in progress is not finalized: its header keeps
        // frameCount = 0 and the PC tools compute it from the file size.
        OSD::Stop(CaptureCallback);
        return 0;
    }
}
