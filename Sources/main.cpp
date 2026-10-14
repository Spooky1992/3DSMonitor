#include <3ds.h>
#include <CTRPluginFramework.hpp>

namespace CTRPluginFramework
{
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
        PluginMenu *menu = new PluginMenu("3DSMonitor", 0, 0, 1,
                                          "Gameplay recorder for Old 3DS.\nPhase 0: hello world.");

        menu->SynchronizeWithFrame(true);

        *menu += new MenuEntry("Hello", nullptr, [](MenuEntry *entry)
        {
            MessageBox("3DSMonitor", "Hello from 3DSMonitor!")();
        });

        OSD::Notify("3DSMonitor loaded");

        menu->Run();

        delete menu;
        return 0;
    }
}
