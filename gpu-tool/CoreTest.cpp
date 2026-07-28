// ===========================================================================
//  CoreTest - console harness for verifying the GpuCore ports against the
//             original PowerShell scripts.
//
//  Exists so the migration can be checked with real output rather than
//  assumed correct. Build with build-test.bat.
//
//  Usage:
//    CoreTest status          -> Limit-GpuUsage.ps1 -Mode Status
//    CoreTest list            -> Set-BackgroundPriority.ps1 -Mode List
//    CoreTest lower           -> ... -Mode Lower
//    CoreTest restore         -> ... -Mode Restore
//    CoreTest pref-reset      -> Set-GpuPreference.ps1 -Mode Reset -SkipDwm
//    CoreTest watch <sec>     -> Limit-GpuUsage.ps1 -Mode Watch
// ===========================================================================

#include "GpuCore.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>
#include <chrono>

static void Print(const std::wstring& line) {
    wprintf(L"%s\n", line.c_str());
    fflush(stdout);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: CoreTest <status|list|lower|restore|pref-reset|watch>\n");
        return 2;
    }

    if (!gpucore::NvmlInit())
        wprintf(L"[warn] NVML init failed; GPU actions will report errors.\n");

    std::wstring cmd = argv[1];
    gpucore::Sink sink = Print;

    if (cmd == L"status") {
        gpucore::ReportStatus(0, sink);
    }
    else if (cmd == L"list" || cmd == L"lower" || cmd == L"restore") {
        gpucore::PriorityOptions o;
        o.mode = (cmd == L"list")  ? gpucore::PrioMode::List
               : (cmd == L"lower") ? gpucore::PrioMode::Lower
                                   : gpucore::PrioMode::Restore;
        gpucore::ApplyBackgroundPriority(o, sink);
    }
    else if (cmd == L"pref-reset") {
        gpucore::PrefOptions o;
        o.mode    = gpucore::GpuPref::Reset;
        o.skipDwm = true;   // never clobber the dwm entry during a test
        gpucore::ApplyGpuPreference(o, sink);
    }
    else if (cmd == L"watch") {
        int seconds = (argc >= 3) ? _wtoi(argv[2]) : 10;

        gpucore::WatchConfig cfg;
        cfg.threshold  = (argc >= 4) ? _wtoi(argv[3]) : 80;
        cfg.intervalMs = 1000;

        // Stop the loop from a helper thread after the requested duration.
        static volatile bool stop = false;
        std::thread timer([seconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            stop = true;
        });

        gpucore::RunWatch(cfg, &stop, sink);
        timer.join();
    }
    else {
        wprintf(L"unknown command: %s\n", cmd.c_str());
        gpucore::NvmlShutdown();
        return 2;
    }

    gpucore::NvmlShutdown();
    return 0;
}
