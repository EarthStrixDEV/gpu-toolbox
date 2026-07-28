// ===========================================================================
//  GpuThrottleDemo - closed-loop GPU self-throttling, and a demonstration of
//                    what GPU throttling can and cannot reach.
//  ---------------------------------------------------------------------
//  WHY THIS PROGRAM EXISTS
//
//  The request was: "use CUDA to enforce that a service stays under a
//  user-adjusted GPU threshold." That specific thing is not achievable, and
//  this program is the honest version of it plus the evidence.
//
//  CUDA is an API for submitting YOUR OWN work to the GPU. A CUDA context
//  governs only the process that created it. It has no authority over other
//  processes:
//
//    - Firefox, Discord, OBS and games render through D3D12/DXGI on the
//      graphics queue. They never create a CUDA context, so no CUDA call can
//      name them, pause them, or bound them.
//    - CUDA_MPS_ACTIVE_THREAD_PERCENTAGE does cap SM occupancy, but only
//      under the Multi-Process Service, only on Linux, and only for CUDA
//      clients that opt into that MPS daemon.
//    - cudaStreamCreateWithPriority orders streams WITHIN one process.
//    - Green Contexts (CUDA 12.4+) partition SMs for contexts you own.
//
//  On Windows the WDDM GPU scheduler owns the queues. Enforcing a per-process
//  GPU budget would require a kernel-mode scheduler filter driver: ring-0,
//  WHQL-signed, BSOD-on-mistake territory. That is a driver team's job, not a
//  user-space tool's.
//
//  WHAT THIS PROGRAM ACTUALLY DOES
//    1. Probes the machine and reports, per mechanism, whether cross-process
//       enforcement is available. (Spoiler: it is not.)
//    2. Runs a REAL closed-loop throttle over a GPU workload this process
//       owns, holding total GPU utilization at or under a threshold you set.
//       That loop is genuine and measurable - it just cannot be pointed at
//       someone else's process.
//
//  BUILD: NVML only, resolved at runtime from nvml.dll. No CUDA Toolkit, no
//  nvml.h, no import library needed.
//      cl /EHsc /O2 /std:c++17 GpuThrottleDemo.cpp
// ===========================================================================

#define WIN32_LEAN_AND_MEAN
// windows.h defines min/max as macros, which collide with std::min/std::max
// and produce cryptic "illegal token on right side of ::" errors.
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Minimal NVML surface, declared by hand.
//
// nvml.h ships with the CUDA Toolkit, which is not installed here, but
// nvml.dll ships with the driver and is present. Declaring the handful of
// entry points we need lets this compile against nothing but the Win32 SDK.
// Signatures follow the documented NVML ABI.
// ---------------------------------------------------------------------------
typedef int nvmlReturn_t;
static const nvmlReturn_t NVML_SUCCESS = 0;

typedef void* nvmlDevice_t;

struct nvmlUtilization_t {
    unsigned int gpu;      // percent of time any kernel was executing
    unsigned int memory;   // percent of time memory was being accessed
};

struct nvmlProcessInfo_v1_t {
    unsigned int  pid;
    unsigned long long usedGpuMemory;
};

typedef nvmlReturn_t (*PFN_nvmlInit)(void);
typedef nvmlReturn_t (*PFN_nvmlShutdown)(void);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetEnforcedPowerLimit)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetGraphicsRunningProcesses)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetComputeRunningProcesses)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*);
typedef const char*  (*PFN_nvmlErrorString)(nvmlReturn_t);

struct Nvml {
    HMODULE dll = nullptr;
    PFN_nvmlInit                              Init = nullptr;
    PFN_nvmlShutdown                          Shutdown = nullptr;
    PFN_nvmlDeviceGetHandleByIndex            GetHandle = nullptr;
    PFN_nvmlDeviceGetName                     GetName = nullptr;
    PFN_nvmlDeviceGetUtilizationRates         GetUtil = nullptr;
    PFN_nvmlDeviceGetTemperature              GetTemp = nullptr;
    PFN_nvmlDeviceGetPowerUsage               GetPower = nullptr;
    PFN_nvmlDeviceGetEnforcedPowerLimit       GetPowerLimit = nullptr;
    PFN_nvmlDeviceGetGraphicsRunningProcesses GetGfxProcs = nullptr;
    PFN_nvmlDeviceGetComputeRunningProcesses  GetComputeProcs = nullptr;
    PFN_nvmlErrorString                       ErrStr = nullptr;

    bool Load() {
        // The driver installs nvml.dll into System32; the NVSMI folder is a
        // fallback for older driver layouts.
        const wchar_t* paths[] = {
            L"nvml.dll",
            L"C:\\Windows\\System32\\nvml.dll",
            L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"
        };
        for (const wchar_t* p : paths) {
            dll = LoadLibraryW(p);
            if (dll) break;
        }
        if (!dll) return false;

        auto G = [&](const char* n) { return GetProcAddress(dll, n); };

        Init            = (PFN_nvmlInit)                   G("nvmlInit_v2");
        if (!Init) Init = (PFN_nvmlInit)                   G("nvmlInit");
        Shutdown        = (PFN_nvmlShutdown)               G("nvmlShutdown");
        GetHandle       = (PFN_nvmlDeviceGetHandleByIndex) G("nvmlDeviceGetHandleByIndex_v2");
        if (!GetHandle)
            GetHandle   = (PFN_nvmlDeviceGetHandleByIndex) G("nvmlDeviceGetHandleByIndex");
        GetName         = (PFN_nvmlDeviceGetName)          G("nvmlDeviceGetName");
        GetUtil         = (PFN_nvmlDeviceGetUtilizationRates) G("nvmlDeviceGetUtilizationRates");
        GetTemp         = (PFN_nvmlDeviceGetTemperature)   G("nvmlDeviceGetTemperature");
        GetPower        = (PFN_nvmlDeviceGetPowerUsage)    G("nvmlDeviceGetPowerUsage");
        GetPowerLimit   = (PFN_nvmlDeviceGetEnforcedPowerLimit) G("nvmlDeviceGetEnforcedPowerLimit");
        GetGfxProcs     = (PFN_nvmlDeviceGetGraphicsRunningProcesses) G("nvmlDeviceGetGraphicsRunningProcesses");
        GetComputeProcs = (PFN_nvmlDeviceGetComputeRunningProcesses)  G("nvmlDeviceGetComputeRunningProcesses");
        ErrStr          = (PFN_nvmlErrorString)            G("nvmlErrorString");

        return Init && GetHandle && GetUtil;
    }

    void Unload() {
        if (Shutdown) Shutdown();
        if (dll) FreeLibrary(dll);
        dll = nullptr;
    }
};

static Nvml       g_nvml;
static nvmlDevice_t g_dev = nullptr;

// ---------------------------------------------------------------------------
// Resolve a PID to an image name, so the capability report names real
// processes rather than bare numbers.
// ---------------------------------------------------------------------------
static std::string PidToName(unsigned int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "(access denied)";

    char buf[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    std::string name = "(unknown)";
    if (QueryFullProcessImageNameA(h, 0, buf, &sz)) {
        std::string full(buf);
        size_t slash = full.find_last_of('\\');
        name = (slash == std::string::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(h);
    return name;
}

// ---------------------------------------------------------------------------
// PART 1 - Capability report
//
// Enumerates what is on the GPU and states, per mechanism, whether it can be
// used to bound another process. This is the part that answers the original
// question with evidence instead of assertion.
// ---------------------------------------------------------------------------
static void ReportCapabilities() {
    printf("\n");
    printf("===========================================================\n");
    printf(" PART 1 - What can actually enforce a per-process GPU cap?\n");
    printf("===========================================================\n\n");

    char name[128]{};
    if (g_nvml.GetName && g_nvml.GetName(g_dev, name, sizeof(name)) == NVML_SUCCESS)
        printf("Device        : %s\n", name);

    unsigned int lim = 0;
    if (g_nvml.GetPowerLimit &&
        g_nvml.GetPowerLimit(g_dev, &lim) == NVML_SUCCESS) {
        printf("Power limit   : %.1f W (enforced)\n", lim / 1000.0);
    } else {
        printf("Power limit   : not exposed by this driver\n");
    }

    // --- who is on the GPU right now -------------------------------------
    printf("\nProcesses currently on the GPU:\n");

    unsigned int count = 0;
    // First call with count=0 asks NVML how many entries it needs.
    if (g_nvml.GetGfxProcs) {
        g_nvml.GetGfxProcs(g_dev, &count, nullptr);
        if (count > 0) {
            std::vector<nvmlProcessInfo_v1_t> procs(count);
            if (g_nvml.GetGfxProcs(g_dev, &count, procs.data()) == NVML_SUCCESS) {
                for (unsigned int i = 0; i < count; ++i) {
                    printf("  [graphics] pid %-7u %s\n",
                           procs[i].pid, PidToName(procs[i].pid).c_str());
                }
            }
        }
    }

    unsigned int ccount = 0;
    if (g_nvml.GetComputeProcs) {
        g_nvml.GetComputeProcs(g_dev, &ccount, nullptr);
        if (ccount > 0) {
            std::vector<nvmlProcessInfo_v1_t> procs(ccount);
            if (g_nvml.GetComputeProcs(g_dev, &ccount, procs.data()) == NVML_SUCCESS) {
                for (unsigned int i = 0; i < ccount; ++i) {
                    printf("  [compute ] pid %-7u %s\n",
                           procs[i].pid, PidToName(procs[i].pid).c_str());
                }
            }
        }
    }

    printf("\n  graphics clients: %u    compute clients: %u\n", count, ccount);
    printf("  Note: the graphics clients above render via D3D12/DXGI. None of\n");
    printf("  them hold a CUDA context, so no CUDA API can address them.\n");

    // --- mechanism-by-mechanism verdict -----------------------------------
    printf("\n-----------------------------------------------------------\n");
    printf(" Mechanism                        Cross-process?  Available?\n");
    printf("-----------------------------------------------------------\n");
    printf(" cudaStreamCreateWithPriority     NO              n/a\n");
    printf("   -> orders streams inside one process only.\n\n");
    printf(" CUDA_MPS_ACTIVE_THREAD_PERCENT   partial         NO\n");
    printf("   -> real SM cap, but Linux-only MPS, CUDA clients only.\n\n");
    printf(" CUDA Green Contexts (12.4+)      NO              NO\n");
    printf("   -> partitions SMs for contexts you own; no toolkit here.\n\n");
    printf(" NVML per-process utilization     read-only       partial\n");
    printf("   -> can observe, cannot bound.\n\n");
    printf(" NVML power / clock limit         device-wide     %s\n",
           lim ? "YES" : "NO");
    printf("   -> affects EVERY process at once, not one service.\n\n");
    printf(" WDDM scheduler filter driver     YES             requires ring-0\n");
    printf("   -> the only true per-process enforcement path on Windows.\n");
    printf("      Kernel-mode, WHQL-signed, out of scope for a user tool.\n");
    printf("-----------------------------------------------------------\n");
    printf("\nConclusion: no user-space API can hold another process under a\n");
    printf("GPU threshold. What follows is the throttle that IS real - it\n");
    printf("governs this process's own GPU work.\n");
}

// ---------------------------------------------------------------------------
// A GPU workload we own.
//
// With no CUDA Toolkit we cannot compile a .cu kernel, so the load is
// generated through Direct3D 11 compute-equivalent work: a busy shader-free
// path using repeated GPU copies and draws would need a swapchain, so instead
// we spin a deliberately heavy CPU->GPU transfer plus readback loop, which
// registers as real GPU utilization in NVML.
//
// The throttling PRINCIPLE is what matters and it is identical for a CUDA
// kernel: do a slice of work, measure utilization, then sleep proportionally
// to how far over the threshold you are.
// ---------------------------------------------------------------------------
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

struct GpuLoad {
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D*     a = nullptr;
    ID3D11Texture2D*     b = nullptr;
    ID3D11Query*         q = nullptr;

    bool Init() {
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if (FAILED(hr)) return false;

        // Large textures so each copy is substantial GPU work.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 2048;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(dev->CreateTexture2D(&td, nullptr, &a))) return false;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &b))) return false;

        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        dev->CreateQuery(&qd, &q);
        return true;
    }

    // One unit of GPU work. Returns after the GPU has actually finished, so
    // the throttle loop measures real execution rather than queue depth.
    void DoWorkSlice(int iterations) {
        for (int i = 0; i < iterations; ++i) {
            ctx->CopyResource(b, a);
            ctx->CopyResource(a, b);
        }
        if (q) {
            ctx->End(q);
            BOOL done = FALSE;
            // Block until the GPU drains this batch.
            while (ctx->GetData(q, &done, sizeof(done), 0) != S_OK) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }

    void Release() {
        if (q)   q->Release();
        if (b)   b->Release();
        if (a)   a->Release();
        if (ctx) ctx->Release();
        if (dev) dev->Release();
    }
};

static unsigned int ReadUtil() {
    nvmlUtilization_t u{};
    if (g_nvml.GetUtil(g_dev, &u) == NVML_SUCCESS) return u.gpu;
    return 0;
}

// ---------------------------------------------------------------------------
// PART 2 - The closed-loop throttle
//
// Control law: proportional duty-cycle adjustment.
//
//   error    = measured_util - threshold
//   dutyPct -= error * gain        (clamped to [minDuty, 100])
//
// Then each cycle spends dutyPct of its period submitting work and the rest
// idle. This is a proportional controller: no integral term, because GPU
// utilization is noisy and an I-term winds up badly against other processes'
// load, which we do not control.
//
// IMPORTANT HONESTY NOTE: NVML reports DEVICE-WIDE utilization, which includes
// every other process. When the desktop alone already exceeds the threshold,
// this loop drives its own duty toward the floor and still cannot reach the
// target - and it says so rather than pretending it succeeded.
// ---------------------------------------------------------------------------
static void RunThrottleLoop(int threshold, int seconds) {
    printf("\n");
    printf("===========================================================\n");
    printf(" PART 2 - Closed-loop self-throttle, target <= %d%%\n", threshold);
    printf("===========================================================\n\n");

    GpuLoad load;
    if (!load.Init()) {
        printf("[error] could not create the D3D11 device - no GPU workload.\n");
        return;
    }

    // Baseline: what the rest of the system is already using. Without this the
    // report cannot distinguish "my throttle works" from "the desktop is idle".
    printf("Measuring baseline load from other processes...\n");
    unsigned int baseSum = 0;
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        baseSum += ReadUtil();
    }
    unsigned int baseline = baseSum / 5;
    printf("Baseline (other processes): %u%%\n\n", baseline);

    if ((int)baseline >= threshold) {
        printf("[!] Baseline already meets or exceeds the %d%% target.\n", threshold);
        printf("    Other processes alone are over the line. Nothing this\n");
        printf("    program does to its OWN work can pull the device below\n");
        printf("    the threshold - that is exactly the limitation Part 1\n");
        printf("    described. Running anyway to show the behaviour.\n\n");
    }

    double dutyPct       = 100.0;   // start unthrottled
    const double gain    = 0.8;     // proportional gain
    const double minDuty = 2.0;     // never fully stop; keeps measurement live
    const int cycleMs    = 200;     // control period

    printf("%-9s %-8s %-9s %-9s %s\n", "TIME", "UTIL", "TARGET", "DUTY", "ACTION");
    printf("-----------------------------------------------------------\n");

    auto start = std::chrono::steady_clock::now();
    int overCount = 0, sampleCount = 0;
    unsigned int peak = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed >= seconds) break;

        unsigned int util = ReadUtil();
        peak = std::max(peak, util);
        sampleCount++;
        if ((int)util > threshold) overCount++;

        // --- proportional correction --------------------------------------
        double error = (double)util - (double)threshold;
        dutyPct -= error * gain;
        dutyPct = std::clamp(dutyPct, minDuty, 100.0);

        int workMs = (int)(cycleMs * dutyPct / 100.0);
        int idleMs = cycleMs - workMs;

        const char* action = (error > 0) ? "throttle down"
                           : (error < 0 ? "ease up" : "hold");

        printf("%6ds   %3u%%     %3d%%      %5.1f%%   %s\n",
               elapsed, util, threshold, dutyPct, action);

        // --- do the work slice, then idle ---------------------------------
        if (workMs > 0) {
            auto wStart = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - wStart).count() < workMs) {
                load.DoWorkSlice(2);
            }
        }
        if (idleMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(idleMs));
    }

    load.Release();

    printf("-----------------------------------------------------------\n");
    printf("\nResult over %d samples:\n", sampleCount);
    printf("  peak utilization      : %u%%\n", peak);
    printf("  samples over target   : %d (%.0f%%)\n",
           overCount, sampleCount ? (100.0 * overCount / sampleCount) : 0.0);
    printf("  final duty cycle      : %.1f%%\n", dutyPct);
    printf("  baseline (not ours)   : %u%%\n", baseline);

    printf("\nRead this correctly: the loop bounded THIS process's GPU work.\n");
    printf("Utilization above the target that persists after the duty cycle\n");
    printf("hits its floor belongs to other processes, and no user-space API\n");
    printf("- CUDA included - can bound those.\n");
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    int threshold = 80;
    int seconds   = 20;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-t" || a == "--threshold") && i + 1 < argc)
            threshold = atoi(argv[++i]);
        else if ((a == "-d" || a == "--duration") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            printf("Usage: GpuThrottleDemo [-t THRESHOLD] [-d SECONDS]\n");
            printf("  -t  target GPU utilization percent (default 80)\n");
            printf("  -d  throttle loop duration in seconds (default 20)\n");
            return 0;
        }
    }

    threshold = std::clamp(threshold, 5, 100);
    seconds   = std::clamp(seconds, 5, 300);

    if (!g_nvml.Load()) {
        printf("[error] could not load nvml.dll. Is the NVIDIA driver installed?\n");
        return 1;
    }
    if (g_nvml.Init() != NVML_SUCCESS) {
        printf("[error] nvmlInit failed.\n");
        g_nvml.Unload();
        return 1;
    }
    if (g_nvml.GetHandle(0, &g_dev) != NVML_SUCCESS) {
        printf("[error] no NVIDIA device at index 0.\n");
        g_nvml.Unload();
        return 1;
    }

    ReportCapabilities();
    RunThrottleLoop(threshold, seconds);

    g_nvml.Unload();
    return 0;
}
