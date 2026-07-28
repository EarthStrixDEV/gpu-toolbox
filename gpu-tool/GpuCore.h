// ===========================================================================
//  GpuCore.h - native replacements for the three PowerShell scripts
//  ---------------------------------------------------------------------
//  Ports Limit-GpuUsage.ps1, Set-GpuPreference.ps1 and
//  Set-BackgroundPriority.ps1 into C++ so GpuToolbox.exe no longer spawns
//  powershell.exe. That removes three whole classes of failure the PS1
//  versions actually hit during development:
//    - execution-policy blocks
//    - the ANSI/UTF-8 mojibake that broke Set-GpuPreference.ps1 outright
//    - process spawn latency on every button press
//
//  Every function reports progress through a caller-supplied sink so the GUI
//  can stream output into its pane exactly as it did with captured stdout.
// ===========================================================================

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>
#include <functional>

namespace gpucore {

// Output sink: one line at a time, already widened.
using Sink = std::function<void(const std::wstring&)>;

// ---------------------------------------------------------------------------
// NVML - loaded at runtime from nvml.dll (ships with the driver). No CUDA
// Toolkit, no nvml.h and no import library required.
// ---------------------------------------------------------------------------
struct GpuStats {
    bool         valid        = false;
    std::wstring name;
    unsigned int utilization  = 0;   // percent
    unsigned int temperature  = 0;   // Celsius
    double       powerDraw    = 0.0; // Watts
    double       powerLimit   = 0.0; // Watts, 0 when not exposed
    double       powerMax     = 0.0; // Watts, 0 when not exposed
    unsigned int graphicsClk  = 0;   // MHz
};

struct GpuProcess {
    unsigned int pid = 0;
    std::wstring name;
    bool         isCompute = false;
};

// Initialise / shut down the NVML binding. Init is idempotent.
bool NvmlInit();
void NvmlShutdown();
bool NvmlAvailable();

// True when the driver exposes a SETTABLE power limit. GeForce and laptop
// parts commonly do not: the board/vBIOS owns power behaviour and
// `nvidia-smi -pl` answers "not supported in current scope" even when run
// elevated. The GUI gates its Power Limit button on this.
bool PowerLimitSupported();

// True when graphics clocks can be locked (`nvidia-smi -lgc`). Verified
// independently of the power limit: on hardware where -pl is refused outright,
// -lgc frequently still works, and clock locking is then the only usable lever
// for holding down heat and power draw.
bool ClockLimitSupported();

bool  QueryStats(unsigned int gpuIndex, GpuStats& out);
std::vector<GpuProcess> QueryGpuProcesses(unsigned int gpuIndex);

// Port of: Limit-GpuUsage.ps1 -Mode Status
void ReportStatus(unsigned int gpuIndex, const Sink& sink);

// ---------------------------------------------------------------------------
// Watch - port of Limit-GpuUsage.ps1 -Mode Watch
//
// The PS1 version had to run in its own console window because an unbounded
// polling loop cannot be captured through a pipe (no EOF). In-process there
// is no such constraint: it runs on a worker thread and streams into the
// output pane, and can be stopped from the UI.
//
// Keeps the episode tracking and hysteresis from the PS1 rewrite: an episode
// opens after ConsecutiveHits polls at/above the threshold and closes only
// after utilization sits clearly below it, so a load hovering at the line
// does not thrash.
// ---------------------------------------------------------------------------
struct WatchConfig {
    unsigned int gpuIndex        = 0;
    int          threshold       = 80;
    int          intervalMs      = 2000;
    int          consecutiveHits = 3;
    int          clearMargin     = 10;
    std::wstring logPath;          // empty -> %LOCALAPPDATA%\gpu-watchdog.log
};

// Runs until *stopFlag becomes true. Blocking; call on a worker thread.
void RunWatch(const WatchConfig& cfg, const volatile bool* stopFlag, const Sink& sink);

// ---------------------------------------------------------------------------
// Per-app GPU preference - port of Set-GpuPreference.ps1
//
// Writes HKCU\Software\Microsoft\DirectX\UserGpuPreferences, where the value
// NAME is the full exe path and the DATA is "GpuPreference=N;".
// ---------------------------------------------------------------------------
enum class GpuPref {
    PowerSaving     = 1,
    HighPerformance = 2,
    Reset           = 0
};

struct PrefOptions {
    GpuPref      mode        = GpuPref::PowerSaving;
    bool         restartObs  = false;
    bool         skipDwm     = false;
    std::wstring obsPathOverride;   // empty -> auto-detect
};

void ApplyGpuPreference(const PrefOptions& opt, const Sink& sink);

// ---------------------------------------------------------------------------
// Background CPU/IO priority - port of Set-BackgroundPriority.ps1
//
// NOTE: this changes CPU and I/O priority only. It does NOT cap GPU usage;
// Windows exposes no per-process GPU quota. Named accordingly throughout.
//
// Lower saves each process's ORIGINAL priority to a state file, because
// processes do not all start at Normal (browsers run helpers at AboveNormal
// and Idle; TextInputHost sits at High). Restoring a flat "Normal" would
// overwrite those rather than restore them.
// ---------------------------------------------------------------------------
enum class PrioMode { List, Lower, Restore };

struct PriorityOptions {
    PrioMode                  mode       = PrioMode::List;
    bool                      aggressive = false;   // Idle instead of BelowNormal
    std::vector<std::wstring> names;                // empty -> built-in defaults
};

void ApplyBackgroundPriority(const PriorityOptions& opt, const Sink& sink);

// ---------------------------------------------------------------------------
// Hardware power cap - port of Limit-GpuUsage.ps1 -Mode PowerLimit / Reset
//
// Requires administrator rights. NVML has no "set power limit" entry point in
// the subset we bind, and the documented setter is frequently rejected on
// GeForce parts anyway, so these shell out to nvidia-smi.exe - the same tool
// the PS1 used. Both report the failure plainly when the driver refuses.
// ---------------------------------------------------------------------------
void SetPowerLimitPercent(unsigned int gpuIndex, int percentOfMax, const Sink& sink);

// Lock the graphics clock to a ceiling, as a percentage of the card's maximum
// supported graphics clock. This is the working alternative on hardware that
// refuses -pl.
void SetClockLimitPercent(unsigned int gpuIndex, int percentOfMax, const Sink& sink);

void ResetGpuCaps(unsigned int gpuIndex, const Sink& sink);

} // namespace gpucore
