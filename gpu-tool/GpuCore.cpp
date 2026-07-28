// ===========================================================================
//  GpuCore.cpp - implementation of the native script replacements
// ===========================================================================

#include "GpuCore.h"

#include <tlhelp32.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <map>
#include <thread>
#include <chrono>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace gpucore {

// ===========================================================================
//  NVML binding
// ===========================================================================
namespace {

typedef int nvmlReturn_t;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
typedef void* nvmlDevice_t;

struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
struct nvmlProcessInfo_v1_t { unsigned int pid; unsigned long long usedGpuMemory; };

// NVML temperature sensor id for the GPU die.
constexpr int NVML_TEMPERATURE_GPU = 0;
// NVML clock id for the graphics domain.
constexpr int NVML_CLOCK_GRAPHICS  = 0;

typedef nvmlReturn_t (*PFN_Init)(void);
typedef nvmlReturn_t (*PFN_Shutdown)(void);
typedef nvmlReturn_t (*PFN_GetHandle)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*PFN_GetName)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*PFN_GetUtil)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (*PFN_GetTemp)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*PFN_GetPower)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*PFN_GetPowerLimit)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*PFN_GetPowerMax)(nvmlDevice_t, unsigned int*, unsigned int*);
// nvmlDeviceGetPowerManagementLimit reports the SETTABLE management limit.
// Distinct from GetEnforcedPowerLimit, which reports whatever cap is
// currently in force (board/vBIOS included) and therefore succeeds even on
// cards that refuse `nvidia-smi -pl`. Only the former indicates that a power
// limit can actually be changed.
typedef nvmlReturn_t (*PFN_GetPowerMgmtLimit)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*PFN_GetClock)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*PFN_GetProcs)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*);

struct NvmlApi {
    HMODULE dll = nullptr;
    PFN_Init          Init          = nullptr;
    PFN_Shutdown      Shutdown      = nullptr;
    PFN_GetHandle     GetHandle     = nullptr;
    PFN_GetName       GetName       = nullptr;
    PFN_GetUtil       GetUtil       = nullptr;
    PFN_GetTemp       GetTemp       = nullptr;
    PFN_GetPower      GetPower      = nullptr;
    PFN_GetPowerLimit     GetPowerLimit   = nullptr;
    PFN_GetPowerMax       GetPowerMax     = nullptr;
    PFN_GetPowerMgmtLimit GetPowerMgmtLim = nullptr;
    PFN_GetClock      GetClock      = nullptr;
    PFN_GetProcs      GetGfxProcs   = nullptr;
    PFN_GetProcs      GetComProcs   = nullptr;

    bool ready       = false;
    bool initialised = false;
};

NvmlApi g_api;

bool LoadNvml() {
    if (g_api.dll) return g_api.ready;

    const wchar_t* paths[] = {
        L"nvml.dll",
        L"C:\\Windows\\System32\\nvml.dll",
        L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"
    };
    for (const wchar_t* p : paths) {
        g_api.dll = LoadLibraryW(p);
        if (g_api.dll) break;
    }
    if (!g_api.dll) return false;

    auto G = [](const char* n) { return GetProcAddress(g_api.dll, n); };

    // _v2 entry points are the modern ABI; fall back for older drivers.
    g_api.Init = (PFN_Init)G("nvmlInit_v2");
    if (!g_api.Init) g_api.Init = (PFN_Init)G("nvmlInit");

    g_api.GetHandle = (PFN_GetHandle)G("nvmlDeviceGetHandleByIndex_v2");
    if (!g_api.GetHandle) g_api.GetHandle = (PFN_GetHandle)G("nvmlDeviceGetHandleByIndex");

    g_api.Shutdown      = (PFN_Shutdown)     G("nvmlShutdown");
    g_api.GetName       = (PFN_GetName)      G("nvmlDeviceGetName");
    g_api.GetUtil       = (PFN_GetUtil)      G("nvmlDeviceGetUtilizationRates");
    g_api.GetTemp       = (PFN_GetTemp)      G("nvmlDeviceGetTemperature");
    g_api.GetPower      = (PFN_GetPower)     G("nvmlDeviceGetPowerUsage");
    g_api.GetPowerLimit   = (PFN_GetPowerLimit)    G("nvmlDeviceGetEnforcedPowerLimit");
    g_api.GetPowerMax     = (PFN_GetPowerMax)      G("nvmlDeviceGetPowerManagementLimitConstraints");
    g_api.GetPowerMgmtLim = (PFN_GetPowerMgmtLimit)G("nvmlDeviceGetPowerManagementLimit");
    g_api.GetClock      = (PFN_GetClock)     G("nvmlDeviceGetClockInfo");
    g_api.GetGfxProcs   = (PFN_GetProcs)     G("nvmlDeviceGetGraphicsRunningProcesses");
    g_api.GetComProcs   = (PFN_GetProcs)     G("nvmlDeviceGetComputeRunningProcesses");

    g_api.ready = g_api.Init && g_api.GetHandle && g_api.GetUtil;
    return g_api.ready;
}

nvmlDevice_t GetDevice(unsigned int index) {
    if (!g_api.ready) return nullptr;
    nvmlDevice_t d = nullptr;
    if (g_api.GetHandle(index, &d) != NVML_SUCCESS) return nullptr;
    return d;
}

std::wstring Widen(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

// Resolve a PID to its image file name.
std::wstring PidToName(unsigned int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"(access denied)";

    wchar_t buf[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    std::wstring name = L"(unknown)";
    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) {
        std::wstring full(buf);
        size_t slash = full.find_last_of(L'\\');
        name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(h);
    return name;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
bool NvmlInit() {
    if (!LoadNvml()) return false;
    if (g_api.initialised) return true;
    if (g_api.Init() != NVML_SUCCESS) return false;
    g_api.initialised = true;
    return true;
}

void NvmlShutdown() {
    if (g_api.initialised && g_api.Shutdown) g_api.Shutdown();
    g_api.initialised = false;
    if (g_api.dll) { FreeLibrary(g_api.dll); g_api.dll = nullptr; }
    g_api.ready = false;
}

bool NvmlAvailable() { return g_api.ready && g_api.initialised; }

bool PowerLimitSupported() {
    if (!NvmlAvailable() || !g_api.GetPowerMgmtLim) return false;
    nvmlDevice_t d = GetDevice(0);
    if (!d) return false;

    // Deliberately queries the MANAGEMENT limit, not the enforced one. On this
    // class of card the enforced limit reads back fine (85 W) while
    // `nvidia-smi -pl` still answers "not supported in current scope" - so
    // testing the enforced value would enable a button the driver rejects.
    unsigned int mw = 0;
    return g_api.GetPowerMgmtLim(d, &mw) == NVML_SUCCESS && mw > 0;
}

bool QueryStats(unsigned int gpuIndex, GpuStats& out) {
    out = GpuStats{};
    if (!NvmlAvailable()) return false;

    nvmlDevice_t d = GetDevice(gpuIndex);
    if (!d) return false;

    char nameBuf[128]{};
    if (g_api.GetName && g_api.GetName(d, nameBuf, sizeof(nameBuf)) == NVML_SUCCESS)
        out.name = Widen(nameBuf);

    nvmlUtilization_t u{};
    if (g_api.GetUtil(d, &u) == NVML_SUCCESS) out.utilization = u.gpu;

    unsigned int t = 0;
    if (g_api.GetTemp && g_api.GetTemp(d, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS)
        out.temperature = t;

    unsigned int mw = 0;
    if (g_api.GetPower && g_api.GetPower(d, &mw) == NVML_SUCCESS)
        out.powerDraw = mw / 1000.0;

    // Report the SETTABLE limit, matching what the PS1 read from
    // `power.limit`. Leaving this at the enforced value made Status claim a
    // limit existed on a card that refuses to set one.
    mw = 0;
    if (g_api.GetPowerMgmtLim && g_api.GetPowerMgmtLim(d, &mw) == NVML_SUCCESS)
        out.powerLimit = mw / 1000.0;

    unsigned int lo = 0, hi = 0;
    if (g_api.GetPowerMax && g_api.GetPowerMax(d, &lo, &hi) == NVML_SUCCESS)
        out.powerMax = hi / 1000.0;

    unsigned int clk = 0;
    if (g_api.GetClock && g_api.GetClock(d, NVML_CLOCK_GRAPHICS, &clk) == NVML_SUCCESS)
        out.graphicsClk = clk;

    out.valid = true;
    return true;
}

std::vector<GpuProcess> QueryGpuProcesses(unsigned int gpuIndex) {
    std::vector<GpuProcess> result;
    if (!NvmlAvailable()) return result;

    nvmlDevice_t d = GetDevice(gpuIndex);
    if (!d) return result;

    // NVML convention: call with count=0 to learn the required size.
    auto collect = [&](PFN_GetProcs fn, bool compute) {
        if (!fn) return;
        unsigned int count = 0;
        fn(d, &count, nullptr);
        if (count == 0) return;

        std::vector<nvmlProcessInfo_v1_t> buf(count);
        if (fn(d, &count, buf.data()) != NVML_SUCCESS) return;

        for (unsigned int i = 0; i < count; ++i) {
            GpuProcess p;
            p.pid       = buf[i].pid;
            p.name      = PidToName(buf[i].pid);
            p.isCompute = compute;
            result.push_back(p);
        }
    };

    collect(g_api.GetGfxProcs, false);
    collect(g_api.GetComProcs, true);
    return result;
}

// ---------------------------------------------------------------------------
// Status - port of Limit-GpuUsage.ps1 -Mode Status
// ---------------------------------------------------------------------------
void ReportStatus(unsigned int gpuIndex, const Sink& sink) {
    if (!NvmlAvailable()) {
        sink(L"[error] NVML unavailable - is the NVIDIA driver installed?");
        return;
    }

    GpuStats s;
    if (!QueryStats(gpuIndex, s)) {
        sink(Fmt(L"[error] no NVIDIA device at index %u", gpuIndex));
        return;
    }

    sink(L"");
    sink(Fmt(L"GPU %u          : %s", gpuIndex, s.name.c_str()));
    sink(Fmt(L"Utilization    : %u %%", s.utilization));
    sink(Fmt(L"Temperature    : %u C",  s.temperature));
    sink(Fmt(L"Power draw     : %.2f W", s.powerDraw));

    if (s.powerLimit > 0.0)
        sink(Fmt(L"Power limit    : %.1f W  (max %.1f W)", s.powerLimit, s.powerMax));
    else
        sink(Fmt(L"Power limit    : [N/A]  (max %.1f W)", s.powerMax));

    sink(Fmt(L"Graphics clock : %u MHz", s.graphicsClk));
    sink(L"");

    auto procs = QueryGpuProcesses(gpuIndex);
    sink(Fmt(L"Processes currently using this GPU (%zu):", procs.size()));
    for (const auto& p : procs) {
        sink(Fmt(L"  [%s] pid %-7u %s",
                 p.isCompute ? L"compute " : L"graphics",
                 p.pid, p.name.c_str()));
    }
}

// ---------------------------------------------------------------------------
// Watch - port of Limit-GpuUsage.ps1 -Mode Watch
// ---------------------------------------------------------------------------
namespace {

std::wstring DefaultLogPath() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    return std::wstring(local) + L"\\gpu-watchdog.log";
}

std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Fmt(L"%04d-%02d-%02d %02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void AppendLog(const std::wstring& path, const std::wstring& line) {
    if (path.empty()) return;

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // Write UTF-8 so the log opens cleanly in any editor.
    std::wstring withEol = line + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, withEol.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::vector<char> utf8(n);
        WideCharToMultiByte(CP_UTF8, 0, withEol.c_str(), -1, utf8.data(), n, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)(n - 1), &written, nullptr);
    }
    CloseHandle(h);
}

} // anonymous namespace

void RunWatch(const WatchConfig& cfg, const volatile bool* stopFlag, const Sink& sink) {
    if (!NvmlAvailable()) {
        sink(L"[error] NVML unavailable.");
        return;
    }

    std::wstring logPath = cfg.logPath.empty() ? DefaultLogPath() : cfg.logPath;

    sink(Fmt(L"Watching GPU %u - threshold %d%%, every %.1f s",
             cfg.gpuIndex, cfg.threshold, cfg.intervalMs / 1000.0));
    sink(L"Log: " + logPath);
    sink(L"");

    // Episode state. An episode is one continuous stretch above the
    // threshold; summarising it as a unit beats logging every poll.
    bool         inEpisode  = false;
    ULONGLONG    epStartMs  = 0;
    unsigned int epPeak     = 0;
    unsigned int epSum      = 0;
    int          epSamples  = 0;
    std::wstring epPeakApps;

    // Hysteresis: only close once utilization is clearly below the threshold,
    // otherwise a load sitting on the line thrashes open/closed.
    const int clearLevel = (std::max)(0, cfg.threshold - cfg.clearMargin);
    int hits = 0, belowStreak = 0;

    while (!(stopFlag && *stopFlag)) {
        GpuStats s;
        if (!QueryStats(cfg.gpuIndex, s)) {
            sink(L"[error] NVML query failed; stopping watch.");
            return;
        }

        const unsigned int util = s.utilization;
        const std::wstring ts   = Timestamp();

        if ((int)util >= cfg.threshold) {
            hits++;
            belowStreak = 0;

            if (!inEpisode && hits >= cfg.consecutiveHits) {
                inEpisode  = true;
                epStartMs  = GetTickCount64();
                epPeak     = 0;
                epSum      = 0;
                epSamples  = 0;
                epPeakApps.clear();

                std::wstring line = Fmt(
                    L"%s  EPISODE START  util=%u%%  temp=%uC  power=%.1fW  (threshold %d%%)",
                    ts.c_str(), util, s.temperature, s.powerDraw, cfg.threshold);
                AppendLog(logPath, line);
                sink(line);
            }

            if (inEpisode) {
                epSum += util;
                epSamples++;

                // Snapshot the process list at peak - the sample most likely
                // to name the actual culprit.
                if (util > epPeak) {
                    epPeak = util;
                    auto procs = QueryGpuProcesses(cfg.gpuIndex);
                    epPeakApps.clear();
                    for (const auto& p : procs) {
                        if (p.isCompute) continue;   // avoid listing each pid twice
                        if (!epPeakApps.empty()) epPeakApps += L" | ";
                        epPeakApps += p.name;
                    }
                    if (epPeakApps.empty()) epPeakApps = L"(none reported)";
                }
            }
        }
        else {
            hits = 0;

            if (inEpisode) {
                if ((int)util <= clearLevel) belowStreak++;
                else                          belowStreak = 0;

                if (belowStreak >= 2) {
                    int dur = (int)((GetTickCount64() - epStartMs) / 1000);
                    int avg = epSamples ? (int)(epSum / epSamples) : 0;

                    std::wstring l1 = Fmt(
                        L"%s  EPISODE END    duration=%ds  peak=%u%%  avg=%d%%",
                        ts.c_str(), dur, epPeak, avg);
                    std::wstring l2 = L"                 at peak: " + epPeakApps;

                    AppendLog(logPath, l1);
                    AppendLog(logPath, l2);
                    sink(l1);
                    sink(l2);

                    inEpisode   = false;
                    belowStreak = 0;
                }
            }
        }

        // Sleep in short slices so a stop request is honoured promptly rather
        // than after a full interval.
        int slept = 0;
        while (slept < cfg.intervalMs && !(stopFlag && *stopFlag)) {
            int chunk = (std::min)(100, cfg.intervalMs - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            slept += chunk;
        }
    }

    sink(L"[stopped] watch ended.");
}

// ===========================================================================
//  Per-app GPU preference - port of Set-GpuPreference.ps1
// ===========================================================================
namespace {

constexpr const wchar_t* kPrefKey =
    L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Find a running process by image name, returning its full path.
std::wstring FindRunningPath(const std::wstring& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::wstring found;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pe.th32ProcessID);
                if (h) {
                    wchar_t buf[MAX_PATH]{};
                    DWORD sz = MAX_PATH;
                    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) found = buf;
                    CloseHandle(h);
                }
                if (!found.empty()) break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::vector<DWORD> FindPidsByName(const std::wstring& exeName) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0)
                pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

std::wstring ExpandEnv(const std::wstring& s) {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, _countof(buf));
    return (n > 0 && n <= _countof(buf)) ? std::wstring(buf) : s;
}

} // anonymous namespace

void ApplyGpuPreference(const PrefOptions& opt, const Sink& sink) {
    struct Target { std::wstring name, path; };
    std::vector<Target> targets;

    // --- dwm.exe ----------------------------------------------------------
    if (!opt.skipDwm) {
        std::wstring dwm = FindRunningPath(L"dwm.exe");
        if (dwm.empty()) dwm = ExpandEnv(L"%WINDIR%\\System32\\dwm.exe");
        if (FileExists(dwm)) targets.push_back({ L"dwm.exe", dwm });
    }

    // --- obs --------------------------------------------------------------
    std::wstring obs;
    if (!opt.obsPathOverride.empty()) {
        // Validate up front: writing a preference for a path that does not
        // exist produces an entry Windows can never match to a real process.
        if (!FileExists(opt.obsPathOverride)) {
            sink(L"[error] ObsPath does not point at an existing file:");
            sink(L"        " + opt.obsPathOverride);
            return;
        }
        obs = opt.obsPathOverride;
        sink(L"[obs  ]  using explicit path override");
    } else {
        obs = FindRunningPath(L"obs64.exe");
        if (obs.empty()) obs = FindRunningPath(L"obs.exe");
        if (obs.empty()) {
            const wchar_t* fallbacks[] = {
                L"C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe",
                L"C:\\Program Files (x86)\\obs-studio\\bin\\64bit\\obs64.exe",
                L"%LOCALAPPDATA%\\Programs\\obs-studio\\bin\\64bit\\obs64.exe",
                L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\OBS Studio\\bin\\64bit\\obs64.exe",
                L"D:\\SteamLibrary\\steamapps\\common\\OBS Studio\\bin\\64bit\\obs64.exe",
                L"E:\\SteamLibrary\\steamapps\\common\\OBS Studio\\bin\\64bit\\obs64.exe"
            };
            for (const wchar_t* f : fallbacks) {
                std::wstring cand = ExpandEnv(f);
                if (FileExists(cand)) { obs = cand; break; }
            }
        }
    }
    if (!obs.empty()) targets.push_back({ L"obs.exe", obs });

    if (targets.empty()) {
        sink(L"[warn] no target process found - nothing to do.");
        sink(L"       If OBS is installed somewhere unusual, set the path explicitly.");
        return;
    }
    if (obs.empty())
        sink(L"[warn] OBS was not found; set its path explicitly to include it.");

    // --- write the registry ----------------------------------------------
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kPrefKey, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                              nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        sink(Fmt(L"[error] cannot open registry key (%ld)", rc));
        return;
    }

    const std::wstring data =
        (opt.mode == GpuPref::PowerSaving)     ? L"GpuPreference=1;" :
        (opt.mode == GpuPref::HighPerformance) ? L"GpuPreference=2;" : L"";

    for (const auto& t : targets) {
        if (opt.mode == GpuPref::Reset) {
            LONG dr = RegDeleteValueW(key, t.path.c_str());
            if (dr == ERROR_SUCCESS) sink(L"[reset]  " + t.path);
            else                     sink(L"[skip ]  " + t.path + L" (no preference was set)");
            continue;
        }

        LONG sr = RegSetValueExW(key, t.path.c_str(), 0, REG_SZ,
                                 (const BYTE*)data.c_str(),
                                 (DWORD)((data.size() + 1) * sizeof(wchar_t)));
        if (sr == ERROR_SUCCESS) {
            sink(L"[set  ]  " + t.path);
            sink(L"         -> " + data);
        } else {
            sink(Fmt(L"[error]  %s (%ld)", t.path.c_str(), sr));
        }
    }
    RegCloseKey(key);

    // --- restart OBS ------------------------------------------------------
    // GpuPreference is read when a process first creates its D3D device, so a
    // running instance keeps the old GPU until restarted. dwm.exe is never
    // touched: terminating DWM breaks the desktop session.
    if (opt.restartObs) {
        std::vector<DWORD> pids = FindPidsByName(L"obs64.exe");
        for (DWORD p : FindPidsByName(L"obs.exe")) pids.push_back(p);

        if (pids.empty()) {
            sink(L"");
            sink(L"OBS is not running - it will pick up the new value next launch.");
        } else {
            sink(L"");
            sink(L"Closing OBS (any recording or stream in progress will stop)...");
            for (DWORD pid : pids) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); }
            }
            sink(L"Closed - reopen OBS for the new preference to take effect.");
        }
    }

    if (opt.mode != GpuPref::Reset) {
        sink(L"");
        sink(L"--- Notes ---");
        sink(L"dwm.exe : this setting usually has no real effect. DWM renders on the");
        sink(L"          GPU that drives the display; if the monitor cable goes into");
        sink(L"          the NVIDIA card, DWM stays on NVIDIA regardless.");
        sink(L"obs.exe : on Power Saving the NVENC encoder becomes unavailable.");
        sink(L"          Switch to x264 or QuickSync under Settings > Output first.");
    }
}

// ===========================================================================
//  Background CPU/IO priority - port of Set-BackgroundPriority.ps1
// ===========================================================================
namespace {

// I/O priority lives behind NtSetInformationProcess, which has no import
// library entry - resolved dynamically. Undocumented but stable; every call
// is failure-tolerant.
typedef LONG (WINAPI *PFN_NtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
constexpr ULONG ProcessIoPriority = 33;

PFN_NtSetInformationProcess GetNtSetInfo() {
    static PFN_NtSetInformationProcess fn = []() -> PFN_NtSetInformationProcess {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (!nt) return nullptr;
        return (PFN_NtSetInformationProcess)GetProcAddress(nt, "NtSetInformationProcess");
    }();
    return fn;
}

bool SetIoPriority(HANDLE h, ULONG value) {
    auto fn = GetNtSetInfo();
    if (!fn) return false;
    ULONG v = value;
    return fn(h, ProcessIoPriority, &v, sizeof(v)) == 0;
}

const wchar_t* PriorityName(DWORD cls) {
    switch (cls) {
        case IDLE_PRIORITY_CLASS:         return L"Idle";
        case BELOW_NORMAL_PRIORITY_CLASS: return L"BelowNormal";
        case NORMAL_PRIORITY_CLASS:       return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return L"AboveNormal";
        case HIGH_PRIORITY_CLASS:         return L"High";
        case REALTIME_PRIORITY_CLASS:     return L"RealTime";
        default:                          return L"Unknown";
    }
}

DWORD PriorityFromName(const std::wstring& n) {
    if (n == L"Idle")        return IDLE_PRIORITY_CLASS;
    if (n == L"BelowNormal") return BELOW_NORMAL_PRIORITY_CLASS;
    if (n == L"Normal")      return NORMAL_PRIORITY_CLASS;
    if (n == L"AboveNormal") return ABOVE_NORMAL_PRIORITY_CLASS;
    if (n == L"High")        return HIGH_PRIORITY_CLASS;
    if (n == L"RealTime")    return REALTIME_PRIORITY_CLASS;
    return NORMAL_PRIORITY_CLASS;
}

// Default background set - the usual CPU-hungry background offenders.
const wchar_t* kDefaultNames[] = {
    L"firefox.exe", L"msedgewebview2.exe", L"chrome.exe", L"brave.exe",
    L"Discord.exe", L"NVIDIA Overlay.exe", L"NVIDIA Web Helper.exe",
    L"EPConsole.exe", L"SearchHost.exe", L"CrossDeviceResume.exe",
    L"TextInputHost.exe", L"OneDrive.exe", L"Widgets.exe", L"WidgetService.exe"
};

// Never touched. Lowering a critical system process degrades the whole
// desktop, and obs64/the game are precisely what we are protecting.
const wchar_t* kProtected[] = {
    L"dwm.exe", L"csrss.exe", L"winlogon.exe", L"services.exe", L"lsass.exe",
    L"smss.exe", L"wininit.exe", L"System", L"Idle", L"Registry",
    L"obs64.exe", L"obs.exe", L"powershell.exe", L"pwsh.exe",
    L"WindowsTerminal.exe", L"GpuToolbox.exe"
};

bool IsProtected(const std::wstring& name) {
    for (const wchar_t* p : kProtected)
        if (_wcsicmp(name.c_str(), p) == 0) return true;
    return false;
}

std::wstring StateFilePath() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    return std::wstring(local) + L"\\gpu-bgpriority-state.txt";
}

struct SavedEntry { std::wstring name; std::wstring priority; };

// State file is a simple "pid|name|priority" line format - deliberately not
// JSON, to avoid pulling in a parser for three fields.
void WriteState(const std::map<DWORD, SavedEntry>& saved) {
    std::wstring path = StateFilePath();
    if (path.empty()) return;

    std::wstring body;
    for (const auto& kv : saved)
        body += Fmt(L"%lu|%s|%s\r\n", kv.first,
                    kv.second.name.c_str(), kv.second.priority.c_str());

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    int n = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::vector<char> utf8(n);
        WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, utf8.data(), n, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)(n - 1), &written, nullptr);
    }
    CloseHandle(h);
}

bool ReadState(std::map<DWORD, SavedEntry>& out) {
    out.clear();
    std::wstring path = StateFilePath();
    if (path.empty()) return false;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::vector<char> buf((size_t)size.QuadPart + 1, 0);
    DWORD got = 0;
    ReadFile(h, buf.data(), (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(h);

    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), -1, nullptr, 0);
    if (n <= 1) return false;
    std::wstring text(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), -1, &text[0], n);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, eol == std::wstring::npos ? eol : eol - pos);
        pos = (eol == std::wstring::npos) ? text.size() : eol + 1;

        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
            line.pop_back();
        if (line.empty()) continue;

        size_t b1 = line.find(L'|');
        if (b1 == std::wstring::npos) continue;
        size_t b2 = line.find(L'|', b1 + 1);
        if (b2 == std::wstring::npos) continue;

        DWORD pid = (DWORD)_wtoi(line.substr(0, b1).c_str());
        out[pid] = { line.substr(b1 + 1, b2 - b1 - 1), line.substr(b2 + 1) };
    }
    return !out.empty();
}

void DeleteState() {
    std::wstring p = StateFilePath();
    if (!p.empty()) DeleteFileW(p.c_str());
}

struct MatchedProc { DWORD pid; std::wstring name; };

std::vector<MatchedProc> MatchProcesses(const std::vector<std::wstring>& names) {
    std::vector<MatchedProc> out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring nm = pe.szExeFile;
            if (IsProtected(nm)) continue;

            bool wanted = false;
            if (names.empty()) {
                for (const wchar_t* d : kDefaultNames)
                    if (_wcsicmp(nm.c_str(), d) == 0) { wanted = true; break; }
            } else {
                for (const auto& d : names)
                    if (_wcsicmp(nm.c_str(), d.c_str()) == 0) { wanted = true; break; }
            }
            if (wanted) out.push_back({ pe.th32ProcessID, nm });
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

} // anonymous namespace

void ApplyBackgroundPriority(const PriorityOptions& opt, const Sink& sink) {
    auto matched = MatchProcesses(opt.names);

    if (matched.empty() && opt.mode != PrioMode::Restore) {
        sink(L"No matching background processes are running.");
        return;
    }

    // ---- List ------------------------------------------------------------
    if (opt.mode == PrioMode::List) {
        sink(L"");
        sink(Fmt(L"%-24s %-8s %s", L"PROCESS", L"PID", L"PRIORITY"));
        sink(std::wstring(52, L'-'));

        int denied = 0;
        for (const auto& m : matched) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m.pid);
            std::wstring pri = L"(access denied)";
            if (h) {
                DWORD c = GetPriorityClass(h);
                if (c) pri = PriorityName(c);
                CloseHandle(h);
            } else {
                denied++;
            }
            sink(Fmt(L"%-24s %-8lu %s", m.name.c_str(), m.pid, pri.c_str()));
        }
        sink(L"");
        sink(Fmt(L"Total matched: %zu   (unreadable: %d)", matched.size(), denied));
        sink(L"Use Lower to apply, Restore to undo.");
        return;
    }

    const DWORD target = opt.aggressive ? IDLE_PRIORITY_CLASS
                                        : BELOW_NORMAL_PRIORITY_CLASS;
    int okCount = 0;
    std::vector<std::wstring> failed;

    // ---- Lower -----------------------------------------------------------
    if (opt.mode == PrioMode::Lower) {
        std::map<DWORD, SavedEntry> saved;

        for (const auto& m : matched) {
            // PROCESS_SET_INFORMATION is the right that actually gets denied;
            // query alone is granted far more freely.
            HANDLE h = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION,
                FALSE, m.pid);
            if (!h) { failed.push_back(Fmt(L"%s (pid %lu)", m.name.c_str(), m.pid)); continue; }

            DWORD orig = GetPriorityClass(h);
            if (!orig) {
                failed.push_back(Fmt(L"%s (pid %lu)", m.name.c_str(), m.pid));
                CloseHandle(h);
                continue;
            }

            // Already at or below target - leave it, so Restore can never
            // promote something that was deliberately idle to begin with.
            if (orig == IDLE_PRIORITY_CLASS ||
                (orig == BELOW_NORMAL_PRIORITY_CLASS && !opt.aggressive)) {
                sink(Fmt(L"[skip    ] %s (pid %lu) already %s",
                         m.name.c_str(), m.pid, PriorityName(orig)));
                CloseHandle(h);
                continue;
            }

            if (SetPriorityClass(h, target)) {
                bool ioOk = SetIoPriority(h, 0);   // 0 = very low
                saved[m.pid] = { m.name, PriorityName(orig) };
                sink(Fmt(L"[%-8s] %s (pid %lu) %s -> %s",
                         ioOk ? L"cpu+io" : L"cpu only",
                         m.name.c_str(), m.pid,
                         PriorityName(orig), PriorityName(target)));
                okCount++;
            } else {
                failed.push_back(Fmt(L"%s (pid %lu)", m.name.c_str(), m.pid));
            }
            CloseHandle(h);
        }

        if (!saved.empty()) {
            WriteState(saved);
            sink(L"");
            sink(L"Saved original priorities to: " + StateFilePath());
        }
    }
    // ---- Restore ---------------------------------------------------------
    else {
        std::map<DWORD, SavedEntry> saved;
        if (!ReadState(saved)) {
            sink(L"[warn] no saved state found - nothing was lowered by this tool.");
            sink(L"       Expected: " + StateFilePath());
            sink(L"       Refusing to blanket-set everything to Normal, which");
            sink(L"       would overwrite priorities this tool never changed.");
            return;
        }

        for (const auto& kv : saved) {
            const DWORD pid = kv.first;
            const auto& e   = kv.second;

            HANDLE h = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION,
                FALSE, pid);
            if (!h) {
                sink(Fmt(L"[gone    ] %s (pid %lu) - exited; priority already reset",
                         e.name.c_str(), pid));
                continue;
            }

            // Guard against PID reuse: if this pid now belongs to a different
            // image, leave it alone.
            wchar_t buf[MAX_PATH]{};
            DWORD sz = MAX_PATH;
            std::wstring nowName;
            if (QueryFullProcessImageNameW(h, 0, buf, &sz)) {
                std::wstring full(buf);
                size_t slash = full.find_last_of(L'\\');
                nowName = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
            }
            if (!nowName.empty() && _wcsicmp(nowName.c_str(), e.name.c_str()) != 0) {
                sink(Fmt(L"[skip    ] pid %lu is now '%s', not '%s'",
                         pid, nowName.c_str(), e.name.c_str()));
                CloseHandle(h);
                continue;
            }

            DWORD want = PriorityFromName(e.priority);
            if (SetPriorityClass(h, want)) {
                bool ioOk = SetIoPriority(h, 2);   // 2 = normal
                sink(Fmt(L"[%-8s] %s (pid %lu) -> %s",
                         ioOk ? L"cpu+io" : L"cpu only",
                         e.name.c_str(), pid, e.priority.c_str()));
                okCount++;
            } else {
                failed.push_back(Fmt(L"%s (pid %lu)", e.name.c_str(), pid));
            }
            CloseHandle(h);
        }

        DeleteState();
    }

    sink(L"");
    sink(Fmt(L"Changed: %d", okCount));

    if (!failed.empty()) {
        sink(L"");
        sink(L"Access denied (run as Administrator to include these):");
        for (const auto& f : failed) sink(L"  " + f);
    }

    if (opt.mode == PrioMode::Lower) {
        sink(L"");
        sink(L"--- Reality check ---");
        sink(L"This changed CPU/IO priority only. It does not cap GPU usage -");
        sink(L"Windows has no per-process GPU quota API. If the bottleneck is");
        sink(L"the GPU itself this will not move the needle. Priority also");
        sink(L"resets when each process restarts.");
    }
}

// ===========================================================================
//  Hardware power cap - port of Limit-GpuUsage.ps1 -Mode PowerLimit / Reset
// ===========================================================================
namespace {

std::wstring FindNvidiaSmi() {
    const wchar_t* candidates[] = {
        L"C:\\Windows\\System32\\nvidia-smi.exe",
        L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe"
    };
    for (const wchar_t* c : candidates)
        if (FileExists(c)) return c;

    // Last resort: let PATH resolve it.
    return L"nvidia-smi.exe";
}

// Run a console command and capture combined stdout+stderr.
bool RunCapture(const std::wstring& cmdLine, std::wstring& out, DWORD* exitCode) {
    out.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    // The read end must not be inherited, or the child holds it open and we
    // never observe EOF.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = wr;
    si.hStdError   = wr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }

    std::string raw;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        raw.append(buf, got);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, 30000);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!raw.empty()) {
        int n = MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        if (n > 0) {
            out.resize(n);
            MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), &out[0], n);
        }
    }
    return true;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION el{};
    DWORD sz = sizeof(el);
    bool result = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &sz)
                  && el.TokenIsElevated;
    CloseHandle(token);
    return result;
}

} // anonymous namespace

void SetPowerLimitPercent(unsigned int gpuIndex, int percentOfMax, const Sink& sink) {
    if (!IsElevated()) {
        sink(L"[error] this action requires administrator rights.");
        return;
    }
    if (!NvmlAvailable()) {
        sink(L"[error] NVML unavailable.");
        return;
    }

    GpuStats s;
    if (!QueryStats(gpuIndex, s) || s.powerMax <= 0.0) {
        sink(L"[error] this card does not report a power-limit range.");
        return;
    }

    const double target = (s.powerMax * percentOfMax) / 100.0;
    sink(Fmt(L"Setting power limit to %.0f W (%d percent of max %.0f W)",
             target, percentOfMax, s.powerMax));

    std::wstring cmd = L"\"" + FindNvidiaSmi() + L"\" -pl "
                     + Fmt(L"%.0f", target) + Fmt(L" --id=%u", gpuIndex);

    std::wstring out;
    DWORD ec = 1;
    if (!RunCapture(cmd, out, &ec)) {
        sink(L"[error] could not launch nvidia-smi.");
        return;
    }
    if (!out.empty()) sink(out);

    if (ec != 0) {
        sink(L"");
        sink(L"[failed] The driver rejected the power limit.");
        sink(L"Many GeForce cards have -pl locked (full support is a");
        sink(L"Quadro/Tesla feature). Use MSI Afterburner's power slider instead.");
    }
}

void ResetGpuCaps(unsigned int gpuIndex, const Sink& sink) {
    if (!IsElevated()) {
        sink(L"[error] this action requires administrator rights.");
        return;
    }

    const std::wstring smi = FindNvidiaSmi();

    GpuStats s;
    if (QueryStats(gpuIndex, s) && s.powerMax > 0.0) {
        sink(L"Restoring power limit to default...");
        std::wstring cmd = L"\"" + smi + L"\" -pl " + Fmt(L"%.0f", s.powerMax)
                         + Fmt(L" --id=%u", gpuIndex);
        std::wstring out;
        DWORD ec = 0;
        RunCapture(cmd, out, &ec);
        if (!out.empty()) sink(out);
    }

    sink(L"Unlocking clocks...");
    std::wstring cmd2 = L"\"" + smi + L"\" -rgc" + Fmt(L" --id=%u", gpuIndex);
    std::wstring out2;
    DWORD ec2 = 0;
    RunCapture(cmd2, out2, &ec2);
    if (!out2.empty()) sink(out2);

    sink(L"Done.");
}

} // namespace gpucore
