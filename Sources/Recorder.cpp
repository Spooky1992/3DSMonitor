#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstdlib>

#include "Common.hpp"
#include "Recorder.hpp"

namespace CTRPluginFramework
{
namespace Recorder
{
    static const u32    kFps = 15;
    static const u32    kMaxSlots = 12;         ///< 12 * 187.5 KB = 2.2 MB
    static const u32    kMinSlots = 3;
    static const u32    kHeapReserve = 512 * 1024; ///< Left free for CTRPF after allocating the ring
    static const u32    kWriterStack = 0x2000;
    // Lower priority (higher value) than a game's main thread (usually 0x30):
    // the writer must never steal CPU time from the game. If it is starved,
    // the ring fills up and frames are dropped, which shows in the log.
    // TODO: tune on console if the log shows drops while the SD card is idle.
    static const u32    kWriterPriority = 0x31;
    static const u64    kMaxFileBytes = 0xFFF00000ULL; ///< FAT32 limit is 4 GiB - 1

    // --- Shared state. Accessed with __atomic builtins where noted. ---

    static u8 *     g_ring = nullptr;
    static u32      g_slots = 0;

    static u32      g_head = 0;         ///< Frames produced. Written by OnFrame (release), read by writer (acquire)
    static u32      g_tail = 0;         ///< Frames written. Written by writer (release), read by OnFrame (acquire)
    static s32      g_capturing = 0;    ///< OnFrame may capture (seq_cst, see Stop())
    static s32      g_inOnFrame = 0;    ///< OnFrame is running (seq_cst, see Stop())
    static s32      g_stopWriter = 0;   ///< Writer must exit once the ring is empty
    static s32      g_writerDone = 0;   ///< Writer has exited

    // Written by OnFrame only (read by the plugin thread after Stop()).
    static u64      g_nextTick = 0;
    static u64      g_interval = 0;
    static u32      g_dropped = 0;      ///< Ring full when a frame was due
    static u32      g_unreadable = 0;   ///< Framebuffer not readable / unknown format
    static u32      g_srcFormat = 0;
    static u64      g_convertTicks = 0;
    static u64      g_convertMax = 0;

    // Written by the writer only (read by the plugin thread once g_writerDone).
    static File     g_file;
    static u64      g_bytes = 0;
    static u64      g_writeTicks = 0;
    static u32      g_writeCalls = 0;
    static int      g_writeError = 0;

    // Plugin thread only.
    static bool         g_session = false;
    static ThreadEx *   g_writer = nullptr;
    static std::string  g_name;
    static u64          g_startTick = 0;
    static u64          g_stopTick = 0;

    template <typename T>
    static inline T     Load(T *p, int order = __ATOMIC_ACQUIRE) { return __atomic_load_n(p, order); }
    template <typename T>
    static inline void  Store(T *p, T v, int order = __ATOMIC_RELEASE) { __atomic_store_n(p, v, order); }

    static inline u8 *  Slot(u32 index) { return g_ring + (index % g_slots) * kFrameBytes; }

    bool    Init(void)
    {
        for (u32 slots = kMaxSlots; slots >= kMinSlots; --slots)
        {
            u8 *ring = static_cast<u8 *>(malloc(slots * kFrameBytes));
            if (!ring)
                continue;

            // Make sure CTRPF still has room to work (menus, notifications...)
            void *reserve = malloc(kHeapReserve);
            if (reserve)
            {
                free(reserve);
                g_ring = ring;
                g_slots = slots;
                return true;
            }
            free(ring);
        }
        return false;
    }

    u32     SlotCount(void) { return g_slots; }

    u16 *   IdleBuffer(void)
    {
        return g_session ? nullptr : reinterpret_cast<u16 *>(g_ring);
    }

    bool    IsRecording(void) { return g_session; }

    // Writer thread: writes every contiguous run of ready slots in one call.
    static void     WriterMain(void *)
    {
        u32 tail = g_tail;

        while (true)
        {
            const u32 head = Load(&g_head);

            if (head == tail)
            {
                if (Load(&g_stopWriter))
                    break;
                svcSleepThread(4000000); ///< 4 ms
                continue;
            }

            const u32 index = tail % g_slots;
            u32 count = head - tail;
            if (index + count > g_slots)
                count = g_slots - index; ///< Stop at the end of the ring, the rest comes next loop

            const u32 bytes = count * kFrameBytes;
            if (g_bytes + bytes > kMaxFileBytes)
            {
                g_writeError = -100; ///< File size limit reached
                break;
            }

            const u64 start = svcGetSystemTick();
            const int res = g_file.Write(Slot(index), bytes);
            g_writeTicks += svcGetSystemTick() - start;
            ++g_writeCalls;

            if (res != File::SUCCESS)
            {
                g_writeError = res; ///< SD card full, removed...
                break;
            }

            g_bytes += bytes;
            tail += count;
            Store(&g_tail, tail);
        }

        // On error, stop capturing right away (Stop() will do the rest).
        Store(&g_capturing, (s32)0, __ATOMIC_SEQ_CST);
        Store(&g_writerDone, (s32)1);
    }

    int     Start(void)
    {
        if (g_session || !g_ring)
            return -1;

        g_name = TimestampName("rec");

        int res = File::Open(g_file, g_recDir + "/" + g_name, File::WRITE | File::CREATE | File::TRUNCATE);
        if (res != File::SUCCESS)
            return res;

        // Placeholder header, rewritten with the real counts by Poll(). If the
        // game exits while recording, frameCount stays 0 and the PC tools
        // compute it from the file size.
        RecHeader hdr;
        InitHeader(hdr, 0, kFps, 0, 0);
        res = g_file.Write(&hdr, sizeof(hdr));
        if (res != File::SUCCESS)
        {
            g_file.Close();
            return res;
        }

        g_head = g_tail = 0;
        g_stopWriter = g_writerDone = 0;
        g_nextTick = 0;
        g_interval = SYSCLOCK_ARM11 / kFps;
        g_dropped = g_unreadable = g_srcFormat = 0;
        g_convertTicks = g_convertMax = 0;
        g_bytes = g_writeTicks = 0;
        g_writeCalls = 0;
        g_writeError = 0;

        g_writer = new ThreadEx(WriterMain, kWriterStack, kWriterPriority, -1);
        res = g_writer->Start(nullptr);
        if (R_FAILED(res))
        {
            delete g_writer;
            g_writer = nullptr;
            g_file.Close();
            return res;
        }

        g_session = true;
        g_startTick = svcGetSystemTick();
        __atomic_thread_fence(__ATOMIC_SEQ_CST); ///< Counters above are visible before OnFrame starts
        Store(&g_capturing, (s32)1, __ATOMIC_SEQ_CST);
        return 0;
    }

    void    Stop(void)
    {
        if (!g_session)
            return;

        // Dekker-style handshake with OnFrame (both sides seq_cst): once
        // g_capturing is 0 and g_inOnFrame was seen 0, OnFrame will not
        // publish any new frame.
        Store(&g_capturing, (s32)0, __ATOMIC_SEQ_CST);
        while (Load(&g_inOnFrame, __ATOMIC_SEQ_CST))
            svcSleepThread(1000000); ///< 1 ms

        if (!g_stopTick)
            g_stopTick = svcGetSystemTick();
        Store(&g_stopWriter, (s32)1);
    }

    void    Poll(void)
    {
        if (!g_session)
            return;

        // Writer stopped on its own (write error): stop capturing too.
        if (Load(&g_writerDone) && !Load(&g_stopWriter))
            Stop();

        if (!Load(&g_writerDone))
            return;

        g_writer->Join(true);
        delete g_writer;
        g_writer = nullptr;

        const u32 frames = (u32)(g_bytes / kFrameBytes);
        const float seconds = TicksToMs(g_stopTick - g_startTick) / 1000.f;

        // Final header with the real counts.
        RecHeader hdr;
        InitHeader(hdr, g_srcFormat, kFps, frames, g_dropped + g_unreadable);
        int res = g_file.Seek(0, File::SET);
        if (res == File::SUCCESS)
            res = g_file.Write(&hdr, sizeof(hdr));
        int closeRes = g_file.Close();
        if (res == File::SUCCESS)
            res = closeRes;

        const float writeSec = TicksToMs(g_writeTicks) / 1000.f;
        Log("rec %s frames=%lu dropped=%lu unreadable=%lu duration=%.1fs fmt=%lu slots=%lu",
            g_name.c_str(), frames, g_dropped, g_unreadable, seconds, g_srcFormat, g_slots);
        Log("rec %s size=%lluKB sd=%.0fKB/s (busy %.0f%%, %lu writes) convert avg=%.2fms max=%.2fms writeErr=%d headerRes=%d",
            g_name.c_str(), g_bytes / 1024,
            writeSec > 0.f ? (float)(g_bytes / 1024) / writeSec : 0.f,
            seconds > 0.f ? writeSec * 100.f / seconds : 0.f, g_writeCalls,
            g_head ? TicksToMs(g_convertTicks) / (float)g_head : 0.f, ///< g_head = frames converted
            TicksToMs(g_convertMax), g_writeError, res);

        if (g_writeError)
            OSD::Notify(Utils::Format("REC stopped: write error %08X", g_writeError), Color::Red);
        else
            OSD::Notify(Utils::Format("REC saved: %lu frames, %lu dropped", frames, g_dropped + g_unreadable));

        g_session = false;
        g_stopTick = 0;
    }

    void    OnFrame(const Screen &screen)
    {
        if (!Load(&g_capturing, __ATOMIC_SEQ_CST))
            return;

        Store(&g_inOnFrame, (s32)1, __ATOMIC_SEQ_CST);
        if (!Load(&g_capturing, __ATOMIC_SEQ_CST))
        {
            Store(&g_inOnFrame, (s32)0, __ATOMIC_SEQ_CST);
            return;
        }

        // Pace captures to kFps whatever the game's frame rate.
        const u64 now = svcGetSystemTick();
        if (now >= g_nextTick)
        {
            // Late by more than one interval (game paused, slow frame...): resync.
            g_nextTick = (g_nextTick == 0 || now - g_nextTick > g_interval) ? now + g_interval
                                                                            : g_nextTick + g_interval;
            const u32 head = g_head;

            if (head - Load(&g_tail) >= g_slots)
                ++g_dropped; ///< Writer is behind: drop, never wait
            else
            {
                u64 ticks;
                g_srcFormat = screen.Format;
                if (ConvertTopScreen(screen, reinterpret_cast<u16 *>(Slot(head)), ticks))
                {
                    g_convertTicks += ticks;
                    if (ticks > g_convertMax)
                        g_convertMax = ticks;
                    Store(&g_head, head + 1);
                }
                else
                    ++g_unreadable;
            }
        }

        Store(&g_inOnFrame, (s32)0, __ATOMIC_SEQ_CST);
    }
}
}
