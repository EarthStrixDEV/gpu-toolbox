// ===========================================================================
//  GpuToolbox - native Win32 GUI launcher for the GPU PowerShell scripts
//  ---------------------------------------------------------------------
//  Pure Win32 API. No MFC, no Qt, no external dependencies.
//
//  WHAT THIS DOES:
//    - All logic runs natively in-process via GpuCore (the PowerShell scripts
//      have been ported to C++); no powershell.exe is spawned
//    - Shows live GPU stats (NVML) and total CPU load (PDH)
//    - Streams job output into a read-only output pane
//    - Relaunches itself elevated only for the actions that need admin
//
//  WHAT THIS DELIBERATELY DOES *NOT* DO:
//    There is no "Optimize GPU/CPU" button. On this machine nvidia-smi
//    reports power.limit = [N/A], meaning the driver refuses -pl, and it is
//    a Laptop GPU whose power/clock behaviour is owned by the vBIOS and
//    Dynamic Boost. A button claiming to "optimize" would either fail
//    silently or imply a fix that never happened. Instead the app DETECTS
//    capability at startup and disables controls the hardware won't honour,
//    explaining why in the output pane.
// ===========================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <pdh.h>
#include <pdhmsg.h>

// Native replacements for the three PowerShell scripts. The GUI now calls
// these directly instead of spawning powershell.exe for every button, which
// removes execution-policy blocks, script-encoding breakage, and spawn
// latency from the interactive path.
#include "GpuCore.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// Enable visual styles so controls don't render in Windows 95 chrome.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
enum : int {
    ID_BTN_STATUS = 1001,
    ID_BTN_WATCH,
    ID_BTN_PREF_POWERSAVE,
    ID_BTN_PREF_PERF,
    ID_BTN_PREF_RESET,
    ID_BTN_POWERLIMIT,
    ID_BTN_GPU_RESET,
    ID_BTN_CLEAR,
    ID_BTN_OPENLOG,
    ID_BTN_BG_LIST,
    ID_BTN_BG_LOWER,
    ID_BTN_BG_RESTORE,
    ID_EDIT_OUTPUT,
    ID_STATIC_STATS,
    ID_SLIDER_THRESHOLD,
    ID_STATIC_THRESHOLD,
    ID_TIMER_STATS = 2001
};

// Threshold slider bounds. Below 50 every idle desktop trips the watchdog;
// above 100 is meaningless.
static const int THRESHOLD_MIN     = 50;
static const int THRESHOLD_MAX     = 100;
static const int THRESHOLD_DEFAULT = 80;

// Custom messages: worker thread finished, re-enable the UI.
#define WM_APP_JOB_DONE   (WM_APP + 1)
#define WM_APP_WATCH_DONE (WM_APP + 2)

// ---------------------------------------------------------------------------
// Globals - small enough app that a handful of module-scope handles is
// clearer than threading a context pointer through every callback.
// ---------------------------------------------------------------------------
static HWND  g_hMain      = nullptr;
static HWND  g_hOutput    = nullptr;
static HWND  g_hStats     = nullptr;
static HWND  g_hSlider    = nullptr;
static HWND  g_hThreshLbl = nullptr;
static HFONT g_hFontUI    = nullptr;
static HFONT g_hFontMono  = nullptr;

// Current threshold from the slider; feeds Watch mode and the live-bar warning.
static int   g_threshold  = THRESHOLD_DEFAULT;

static std::atomic<bool> g_jobRunning{false};   // one job at a time
static std::atomic<bool> g_shuttingDown{false};

// Watch runs on its own worker thread independent of the job slot, so status
// and preference actions stay usable while it polls. Declared volatile (not
// atomic) because RunWatch takes a `const volatile bool*`.
static volatile bool     g_watchStop    = false;
static std::atomic<bool> g_watchRunning{false};

// Forward declaration: ToggleWatch updates the button caption.
static void SetWatchButtonText();

// Capability flags probed once at startup.
static bool g_hasNvidiaSmi   = false;
static bool g_powerLimitOk   = false;   // false when power.limit reads [N/A]


// PDH query for total CPU usage.
static PDH_HQUERY   g_cpuQuery   = nullptr;
static PDH_HCOUNTER g_cpuCounter = nullptr;


// Append text to the output pane. Safe to call from any thread: it marshals
// to the UI thread via SendMessage on the edit control, which is fine because
// EM_REPLACESEL is handled synchronously by the control's own window proc.
static void AppendOutput(const std::wstring& text) {
    if (!g_hOutput || g_shuttingDown) return;

    // Normalise bare \n to \r\n, otherwise a classic EDIT control renders
    // everything on one line.
    std::wstring norm;
    norm.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) norm += L"\r\n";
        else norm += text[i];
    }

    int len = GetWindowTextLengthW(g_hOutput);
    SendMessageW(g_hOutput, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hOutput, EM_REPLACESEL, FALSE, (LPARAM)norm.c_str());
    SendMessageW(g_hOutput, EM_SCROLLCARET, 0, 0);
}

static void AppendLine(const std::wstring& s) { AppendOutput(s + L"\r\n"); }

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------------------
// Capability probe - run once at startup. This is what lets the UI be honest
// instead of offering dead buttons. Reads NVML directly rather than parsing
// nvidia-smi text output.
// ---------------------------------------------------------------------------
static void ProbeCapabilities() {
    g_hasNvidiaSmi = gpucore::NvmlInit() && gpucore::NvmlAvailable();
    if (!g_hasNvidiaSmi) return;

    // Does the driver expose an enforced power limit? GeForce parts commonly
    // do not, which means a power-limit request would be rejected.
    g_powerLimitOk = gpucore::PowerLimitSupported();
}

// ---------------------------------------------------------------------------
// Live stats line: GPU util / temp / power / clock + total CPU load.
// ---------------------------------------------------------------------------
static void UpdateStats() {
    std::wstring line;

    if (g_hasNvidiaSmi) {
        // Direct NVML read - no subprocess, no CSV parsing.
        gpucore::GpuStats s;
        if (gpucore::QueryStats(0, s)) {
            wchar_t tmp[160];
            swprintf_s(tmp, L"GPU  %u%%    %u C    %.1f W    %u MHz",
                       s.utilization, s.temperature, s.powerDraw, s.graphicsClk);
            line += tmp;

            // Mark when the live reading is at or past the slider value, so
            // the bar and the watchdog tell the same story.
            if ((int)s.utilization >= g_threshold)
                line += L"   [OVER " + std::to_wstring(g_threshold) + L"%]";
        } else {
            line += L"GPU  (query failed)";
        }
    } else {
        line += L"GPU  NVML unavailable";
    }

    // CPU total via PDH. First sample after PdhOpenQuery always returns
    // PDH_INVALID_DATA because a rate counter needs two samples - that is
    // expected, not an error worth surfacing.
    if (g_cpuQuery) {
        PDH_FMT_COUNTERVALUE val{};
        if (PdhCollectQueryData(g_cpuQuery) == ERROR_SUCCESS &&
            PdhGetFormattedCounterValue(g_cpuCounter, PDH_FMT_DOUBLE,
                                        nullptr, &val) == ERROR_SUCCESS) {
            wchar_t tmp[64];
            swprintf_s(tmp, L"        CPU  %.0f%%", val.doubleValue);
            line += tmp;
        }
    }

    if (g_hStats) SetWindowTextW(g_hStats, line.c_str());
}

// ---------------------------------------------------------------------------
// Enable/disable every action button while a job is in flight.
// ---------------------------------------------------------------------------
// The Watch button doubles as its own stop control, so its caption tracks
// state rather than staying fixed.
static void SetWatchButtonText() {
    HWND h = GetDlgItem(g_hMain, ID_BTN_WATCH);
    if (h) SetWindowTextW(h, g_watchRunning ? L"Stop Watch" : L"Start Watch");
}

static void SetButtonsEnabled(bool enabled) {
    const int ids[] = {
        ID_BTN_STATUS, ID_BTN_WATCH, ID_BTN_PREF_POWERSAVE,
        ID_BTN_PREF_PERF, ID_BTN_PREF_RESET, ID_BTN_POWERLIMIT,
        ID_BTN_GPU_RESET, ID_BTN_BG_LIST, ID_BTN_BG_LOWER,
        ID_BTN_BG_RESTORE
    };
    for (int id : ids) {
        HWND h = GetDlgItem(g_hMain, id);
        if (!h) continue;

        BOOL on = enabled;

        // Watch is its own stop button - it must stay live while a watch is
        // running, otherwise there is no way to end it from the UI.
        if (id == ID_BTN_WATCH && g_watchRunning) on = TRUE;

        // Keep permanently-unavailable controls disabled regardless of job
        // state, so re-enabling never resurrects a dead button.
        if (id == ID_BTN_POWERLIMIT && !g_powerLimitOk) on = FALSE;
        if ((id == ID_BTN_STATUS || id == ID_BTN_WATCH ||
             id == ID_BTN_POWERLIMIT || id == ID_BTN_GPU_RESET) && !g_hasNvidiaSmi) on = FALSE;

        EnableWindow(h, on);
    }
}

// ---------------------------------------------------------------------------
// Run a script on a worker thread and stream the result into the output pane.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Run a native core job on a worker thread, streaming its output into the
// pane. Replaces the old "spawn powershell.exe and capture stdout" path.
// ---------------------------------------------------------------------------
static void RunJobAsync(std::function<void(const gpucore::Sink&)> job,
                        const std::wstring& label)
{
    bool expected = false;
    if (!g_jobRunning.compare_exchange_strong(expected, true)) {
        AppendLine(L"[busy] another job is still running.");
        return;
    }

    SetButtonsEnabled(false);
    AppendLine(L"");
    AppendLine(L"=== " + label + L" ===");

    std::thread([job]() {
        gpucore::Sink sink = [](const std::wstring& line) {
            if (!g_shuttingDown) AppendLine(line);
        };
        job(sink);

        if (!g_shuttingDown)
            PostMessageW(g_hMain, WM_APP_JOB_DONE, 0, 0);
    }).detach();
}

// ---------------------------------------------------------------------------
// Launch a script elevated. Elevation needs a *visible* console because
// ShellExecuteEx with "runas" cannot hand us inherited pipes across the UAC
// boundary - so output goes to its own window rather than our pane. We tell
// the user that instead of pretending output was lost.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Relaunch THIS executable elevated to run one admin-only action.
//
// nvidia-smi's power/clock limits require administrator rights, and a token
// cannot be elevated in place - a new process must be started via the "runas"
// verb. We re-invoke ourselves with a CLI switch rather than shelling out to
// PowerShell, so the migration away from .ps1 stays complete.
// ---------------------------------------------------------------------------
static void RunElevatedSelf(const std::wstring& cliArgs,
                            const std::wstring& label)
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";              // triggers the UAC prompt
    sei.lpFile       = self;
    sei.lpParameters = cliArgs.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    AppendLine(L"");
    AppendLine(L"=== " + label + L" (elevated) ===");
    AppendLine(L"A UAC prompt will appear; output shows in the elevated window.");

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) AppendLine(L"[cancelled] UAC prompt declined.");
        else {
            wchar_t t[96];
            swprintf_s(t, L"[error] elevation failed (%lu).", e);
            AppendLine(t);
        }
        return;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
}

// ---------------------------------------------------------------------------
// Watch mode runs an unbounded polling loop, so it gets its own visible
// console rather than our capture pipe (which would never see EOF).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Watch now runs in-process on a worker thread. The PS1 version needed its own
// console because an unbounded polling loop has no EOF to capture through a
// pipe; with the loop compiled in, output streams straight into the pane and
// the same button stops it.
// ---------------------------------------------------------------------------
static void ToggleWatch() {
    if (g_watchRunning) {
        g_watchStop = true;                 // worker polls this and exits
        AppendLine(L"[stopping] watch will end shortly...");
        return;
    }

    g_watchStop    = false;
    g_watchRunning = true;
    SetWatchButtonText();

    gpucore::WatchConfig cfg;
    cfg.gpuIndex   = 0;
    cfg.threshold  = g_threshold;
    cfg.intervalMs = 2000;

    AppendLine(L"");
    AppendLine(L"=== Watch at " + std::to_wstring(g_threshold) + L"% ===");

    std::thread([cfg]() {
        gpucore::Sink sink = [](const std::wstring& line) {
            if (!g_shuttingDown) AppendLine(line);
        };
        gpucore::RunWatch(cfg, &g_watchStop, sink);

        g_watchRunning = false;
        if (!g_shuttingDown)
            PostMessageW(g_hMain, WM_APP_WATCH_DONE, 0, 0);
    }).detach();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------
static HWND MakeButton(HWND parent, const wchar_t* text, int id,
                       int x, int y, int w, int h)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    return b;
}

static HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND s = CreateWindowExW(0, L"STATIC", text,
                             WS_CHILD | WS_VISIBLE,
                             x, y, w, h, parent, nullptr, nullptr, nullptr);
    SendMessageW(s, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    return s;
}

static void CreateUI(HWND hwnd) {
    const int M = 14;      // margin
    const int BW = 172;    // button width
    const int BH = 30;     // button height
    const int GAP = 8;

    int y = M;

    // ---- live stats -------------------------------------------------------
    MakeLabel(hwnd, L"Live", M, y, 60, 18);
    y += 20;
    g_hStats = CreateWindowExW(0, L"STATIC", L"(reading...)",
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                               M, y, 560, 22, hwnd,
                               (HMENU)(INT_PTR)ID_STATIC_STATS, nullptr, nullptr);
    SendMessageW(g_hStats, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
    y += 34;

    // ---- threshold slider -------------------------------------------------
    MakeLabel(hwnd, L"Alert threshold", M, y, 200, 18);
    g_hThreshLbl = CreateWindowExW(0, L"STATIC", L"80 %",
                                   WS_CHILD | WS_VISIBLE,
                                   M + 160, y, 80, 18, hwnd,
                                   (HMENU)(INT_PTR)ID_STATIC_THRESHOLD,
                                   nullptr, nullptr);
    SendMessageW(g_hThreshLbl, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
    y += 20;

    g_hSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                TBS_HORZ | TBS_AUTOTICKS,
                                M, y, 3*BW + 2*GAP, 30, hwnd,
                                (HMENU)(INT_PTR)ID_SLIDER_THRESHOLD,
                                nullptr, nullptr);
    SendMessageW(g_hSlider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(THRESHOLD_MIN, THRESHOLD_MAX));
    SendMessageW(g_hSlider, TBM_SETTICFREQ, 10, 0);
    SendMessageW(g_hSlider, TBM_SETPOS, TRUE, THRESHOLD_DEFAULT);
    y += 36;

    // ---- diagnostics ------------------------------------------------------
    MakeLabel(hwnd, L"Diagnose  (safe, read-only)", M, y, 400, 18);
    y += 22;
    MakeButton(hwnd, L"GPU Status",       ID_BTN_STATUS, M,               y, BW, BH);
    MakeButton(hwnd, L"Start Watch",      ID_BTN_WATCH,  M + BW + GAP,    y, BW, BH);
    MakeButton(hwnd, L"Open Watch Log",   ID_BTN_OPENLOG, M + 2*(BW+GAP), y, BW, BH);
    y += BH + 16;

    // ---- background priority ---------------------------------------------
    // Named for what it actually changes (CPU/IO priority), NOT "reduce GPU":
    // Windows has no per-process GPU quota, and a button implying otherwise
    // would promise a result it cannot deliver.
    MakeLabel(hwnd, L"Background CPU/IO priority  (does not cap GPU)", M, y, 460, 18);
    y += 22;
    MakeButton(hwnd, L"List Background",  ID_BTN_BG_LIST,    M,              y, BW, BH);
    MakeButton(hwnd, L"Lower Priority",   ID_BTN_BG_LOWER,   M + BW + GAP,   y, BW, BH);
    MakeButton(hwnd, L"Restore Normal",   ID_BTN_BG_RESTORE, M + 2*(BW+GAP), y, BW, BH);
    y += BH + 16;

    // ---- per-app GPU preference ------------------------------------------
    MakeLabel(hwnd, L"App GPU preference  (registry; restarts OBS)", M, y, 460, 18);
    y += 22;
    MakeButton(hwnd, L"OBS -> Power Saving", ID_BTN_PREF_POWERSAVE, M,            y, BW, BH);
    MakeButton(hwnd, L"OBS -> High Perf",    ID_BTN_PREF_PERF, M + BW + GAP,      y, BW, BH);
    MakeButton(hwnd, L"Reset Preference",    ID_BTN_PREF_RESET, M + 2*(BW+GAP),   y, BW, BH);
    y += BH + 16;

    // ---- hardware caps ----------------------------------------------------
    MakeLabel(hwnd, L"Hardware cap  (admin required)", M, y, 460, 18);
    y += 22;
    MakeButton(hwnd, L"Power Limit 90%", ID_BTN_POWERLIMIT, M,            y, BW, BH);
    MakeButton(hwnd, L"Reset GPU Caps",  ID_BTN_GPU_RESET,  M + BW + GAP, y, BW, BH);
    y += BH + 16;

    // ---- output -----------------------------------------------------------
    MakeLabel(hwnd, L"Output", M, y, 100, 18);
    MakeButton(hwnd, L"Clear", ID_BTN_CLEAR, M + 2*(BW+GAP), y - 4, BW, 24);
    y += 22;

    g_hOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                M, y, 560, 240, hwnd,
                                (HMENU)(INT_PTR)ID_EDIT_OUTPUT, nullptr, nullptr);
    SendMessageW(g_hOutput, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
}

// Resize the output pane and stats line to fill the window.
static void LayoutOutput(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int M = 14;

    if (g_hStats) {
        SetWindowPos(g_hStats, nullptr, 0, 0, rc.right - 2*M, 22,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
    if (g_hOutput) {
        RECT orc{};
        GetWindowRect(g_hOutput, &orc);
        POINT tl{ orc.left, orc.top };
        ScreenToClient(hwnd, &tl);
        int h = rc.bottom - tl.y - M;
        if (h < 80) h = 80;
        SetWindowPos(g_hOutput, nullptr, 0, 0, rc.right - 2*M, h,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
}

// ---------------------------------------------------------------------------
// Startup banner - states plainly what the tool will and will not do.
// ---------------------------------------------------------------------------
static void PrintBanner() {
    AppendLine(L"GpuToolbox - native GPU tool (no PowerShell required)");
    AppendLine(L"");

    if (!g_hasNvidiaSmi) {
        AppendLine(L"[!] NVML unavailable. GPU features are disabled.");
        AppendLine(L"    Install the NVIDIA driver, or this machine has no NVIDIA GPU.");
        return;
    }

    gpucore::GpuStats s;
    if (gpucore::QueryStats(0, s)) AppendLine(L"GPU: " + s.name);

    if (!g_powerLimitOk) {
        AppendLine(L"");
        AppendLine(L"[!] This card exposes no enforced power limit.");
        AppendLine(L"    A power-limit request would be rejected by the driver, so");
        AppendLine(L"    that button is disabled. Normal for GeForce / laptop GPUs");
        AppendLine(L"    where the vBIOS owns power behaviour.");
        AppendLine(L"    Use MSI Afterburner if you need a real power cap.");
    }

    AppendLine(L"");
    AppendLine(L"Note: there is no 'auto optimize' button. GPU utilization is a");
    AppendLine(L"result of workload, not a value that can be capped. Run Status and");
    AppendLine(L"Watch first to find what is actually consuming the GPU.");
    AppendLine(L"");
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_hMain = hwnd;
        CreateUI(hwnd);

        ProbeCapabilities();
        SetButtonsEnabled(true);   // applies capability gating internally
        PrintBanner();

        SetTimer(hwnd, ID_TIMER_STATS, 1500, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wp == ID_TIMER_STATS && !g_jobRunning) UpdateStats();
        return 0;

    // Trackbar reports movement via WM_HSCROLL, not WM_COMMAND.
    case WM_HSCROLL: {
        if ((HWND)lp == g_hSlider) {
            g_threshold = (int)SendMessageW(g_hSlider, TBM_GETPOS, 0, 0);
            wchar_t buf[32];
            swprintf_s(buf, L"%d %%", g_threshold);
            SetWindowTextW(g_hThreshLbl, buf);
            UpdateStats();   // refresh the OVER marker immediately
        }
        return 0;
    }

    case WM_SIZE:
        LayoutOutput(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 620;
        mmi->ptMinTrackSize.y = 660;
        return 0;
    }

    case WM_APP_JOB_DONE:
        g_jobRunning = false;
        SetButtonsEnabled(true);
        return 0;

    case WM_APP_WATCH_DONE:
        SetWatchButtonText();
        SetButtonsEnabled(!g_jobRunning);
        return 0;

    case WM_COMMAND: {
        switch (LOWORD(wp)) {

        case ID_BTN_STATUS:
            RunJobAsync([](const gpucore::Sink& s) {
                gpucore::ReportStatus(0, s);
            }, L"GPU Status");
            break;

        case ID_BTN_WATCH:
            ToggleWatch();
            break;

        case ID_BTN_OPENLOG: {
            wchar_t local[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
                std::wstring log = std::wstring(local) + L"\\gpu-watchdog.log";
                if (FileExists(log))
                    ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                  log.c_str(), nullptr, SW_SHOWNORMAL);
                else
                    AppendLine(L"[info] no log yet - run Watch first: " + log);
            }
            break;
        }

        case ID_BTN_PREF_POWERSAVE: {
            int r = MessageBoxW(hwnd,
                L"This closes OBS immediately so the new GPU preference takes effect.\n\n"
                L"Any recording or stream in progress will stop.\n\n"
                L"Also note: on Power Saving the NVENC encoder becomes unavailable, "
                L"so you must switch OBS to x264 or QuickSync afterwards.\n\n"
                L"Continue?",
                L"Confirm - OBS will close", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            if (r == IDYES) {
                RunJobAsync([](const gpucore::Sink& s) {
                    gpucore::PrefOptions o;
                    o.mode       = gpucore::GpuPref::PowerSaving;
                    o.restartObs = true;
                    gpucore::ApplyGpuPreference(o, s);
                }, L"GPU Preference: Power Saving");
            }
            break;
        }

        case ID_BTN_PREF_PERF:
            // No OBS restart here: switching *to* the dGPU has no destructive
            // side effect worth force-closing OBS over.
            RunJobAsync([](const gpucore::Sink& s) {
                gpucore::PrefOptions o;
                o.mode = gpucore::GpuPref::HighPerformance;
                gpucore::ApplyGpuPreference(o, s);
            }, L"GPU Preference: High Performance");
            break;

        case ID_BTN_PREF_RESET:
            RunJobAsync([](const gpucore::Sink& s) {
                gpucore::PrefOptions o;
                o.mode = gpucore::GpuPref::Reset;
                gpucore::ApplyGpuPreference(o, s);
            }, L"GPU Preference: Reset");
            break;

        case ID_BTN_POWERLIMIT:
            if (!g_powerLimitOk) {
                AppendLine(L"[blocked] this card does not expose a settable power limit.");
                break;
            }
            RunElevatedSelf(L"--powerlimit=90", L"Power Limit 90%");
            break;

        case ID_BTN_GPU_RESET:
            RunElevatedSelf(L"--resetcaps", L"Reset GPU Caps");
            break;

        case ID_BTN_BG_LIST:
            RunJobAsync([](const gpucore::Sink& s) {
                gpucore::PriorityOptions o;
                o.mode = gpucore::PrioMode::List;
                gpucore::ApplyBackgroundPriority(o, s);
            }, L"Background processes");
            break;

        case ID_BTN_BG_LOWER:
            RunJobAsync([](const gpucore::Sink& s) {
                gpucore::PriorityOptions o;
                o.mode = gpucore::PrioMode::Lower;
                gpucore::ApplyBackgroundPriority(o, s);
            }, L"Lower background CPU/IO priority");
            break;

        case ID_BTN_BG_RESTORE:
            RunJobAsync([](const gpucore::Sink& s) {
                gpucore::PriorityOptions o;
                o.mode = gpucore::PrioMode::Restore;
                gpucore::ApplyBackgroundPriority(o, s);
            }, L"Restore normal priority");
            break;

        case ID_BTN_CLEAR:
            SetWindowTextW(g_hOutput, L"");
            break;
        }
        return 0;
    }

    case WM_CLOSE:
        g_shuttingDown = true;
        g_watchStop    = true;   // let the watch thread unwind
        KillTimer(hwnd, ID_TIMER_STATS);
        // Give the worker a moment to observe the stop flag before the
        // process tears down the NVML binding it is still calling into.
        if (g_watchRunning) Sleep(250);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Elevated console mode.
//
// The admin-only actions relaunch this exe with a switch. Because the binary
// is /SUBSYSTEM:WINDOWS it owns no console, so attach one to show output.
// Returns true when a CLI action was handled and the GUI should not start.
// ---------------------------------------------------------------------------
static bool RunConsoleMode(LPWSTR cmdLine) {
    std::wstring args = cmdLine ? cmdLine : L"";
    if (args.find(L"--powerlimit") == std::wstring::npos &&
        args.find(L"--resetcaps")  == std::wstring::npos)
        return false;

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();

    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);

    auto sink = [](const std::wstring& line) {
        wprintf(L"%s\n", line.c_str());
        fflush(stdout);
    };

    if (!gpucore::NvmlInit()) {
        wprintf(L"[error] NVML unavailable.\n");
    } else if (args.find(L"--powerlimit") != std::wstring::npos) {
        // Optional "=NN" suffix selects the percentage; default 90.
        int pct = 90;
        size_t eq = args.find(L"--powerlimit=");
        if (eq != std::wstring::npos) {
            int parsed = _wtoi(args.c_str() + eq + 13);
            if (parsed >= 30 && parsed <= 100) pct = parsed;
        }
        gpucore::SetPowerLimitPercent(0, pct, sink);
    } else {
        gpucore::ResetGpuCaps(0, sink);
    }

    gpucore::NvmlShutdown();

    wprintf(L"\nPress Enter to close...\n");
    fflush(stdout);
    (void)getchar();

    if (fp) fclose(fp);
    FreeConsole();
    return true;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // Handle the elevated CLI actions before any window work.
    if (RunConsoleMode(lpCmdLine)) return 0;

    // ICC_BAR_CLASSES registers the trackbar (slider) window class; without it
    // CreateWindowExW(TRACKBAR_CLASSW, ...) silently returns NULL.
    INITCOMMONCONTROLSEX icc{ sizeof(icc),
                              ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    // Fonts: UI font for controls, monospace for stats/output alignment.
    g_hFontUI = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_hFontMono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");

    // PDH counter for total CPU. Failure is non-fatal - we just omit CPU%.
    if (PdhOpenQueryW(nullptr, 0, &g_cpuQuery) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(g_cpuQuery,
                L"\\Processor(_Total)\\% Processor Time",
                0, &g_cpuCounter) == ERROR_SUCCESS) {
            PdhCollectQueryData(g_cpuQuery);   // priming sample
        } else {
            PdhCloseQuery(g_cpuQuery);
            g_cpuQuery = nullptr;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"GpuToolboxWindow";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GPU Toolbox",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 620, 780,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {   // gives Tab-key navigation
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_cpuQuery)  PdhCloseQuery(g_cpuQuery);
    if (g_hFontUI)   DeleteObject(g_hFontUI);
    if (g_hFontMono) DeleteObject(g_hFontMono);
    gpucore::NvmlShutdown();
    return 0;
}
