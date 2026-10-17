#ifndef RECORDER_HPP
#define RECORDER_HPP

#include <3ds.h>

namespace CTRPluginFramework
{
    class Screen;

    // Streams the top screen to /luma/plugins/<TitleID>/rec/rec_*.raw.
    //
    //  game thread (OSD callback)      writer thread             plugin thread
    //  OnFrame(): every 1/fps s,  -->  ring buffer -> SD card    Start() / Stop() / Poll()
    //  convert into a free slot,       (big contiguous writes)   header update, log
    //  or drop the frame if full
    //
    // The ring is a single-producer / single-consumer queue (head written only
    // by OnFrame, tail only by the writer), so OnFrame never blocks the game.
    namespace Recorder
    {
        /// Allocates the ring buffer (plugin thread, once). False if out of memory.
        bool    Init(void);

        u32     SlotCount(void);

        /// Slot 0 of the ring, free to use (e.g. screenshots) while not recording.
        u16 *   IdleBuffer(void);

        /// True from Start() until the file is finalized by Poll().
        bool    IsRecording(void);

        /// Plugin thread. Opens the file and starts recording. Returns 0 or an error.
        int     Start(void);

        /// Plugin thread. Stops capturing; the writer flushes what is left.
        void    Stop(void);

        /// Plugin thread, every loop. Finalizes the file once the writer is done.
        void    Poll(void);

        /// Game thread. Called for the top screen on every frame.
        void    OnFrame(const Screen &screen);
    }
}

#endif
