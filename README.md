# GPU Toolbox

Native Win32 tooling for diagnosing NVIDIA GPU load on Windows, plus an
honest account of what GPU throttling can and cannot actually enforce.

Built and verified on an RTX 3060 Laptop GPU / Windows 11 / MSVC 14.50.

## What it does

- **Live monitoring** — GPU utilization, temperature, power draw and clock via
  NVML, alongside total CPU load via PDH.
- **Watch mode** — polls utilization and logs *episodes* (a continuous stretch
  above a user-set threshold), recording peak, average, duration and the
  process list captured at peak load.
- **Per-app GPU preference** — routes OBS to the integrated or discrete GPU by
  writing `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`.
- **Background CPU/IO priority** — lowers priority of background processes and
  restores their *original* values, not a flat `Normal`.

## What it deliberately does not do

There is no "optimize GPU" button, and no per-process GPU cap.

GPU utilization is the *result* of submitted workload, not a value any
user-space API can bound. On Windows the WDDM scheduler owns the GPU queues;
enforcing a per-process GPU budget would require a kernel-mode scheduler
filter driver. CUDA does not help here either — a CUDA context governs only
the process that created it, and desktop applications render through
D3D12/DXGI without ever holding one.

`GpuThrottleDemo` demonstrates this concretely: it enumerates every process on
the GPU, reports mechanism by mechanism whether cross-process enforcement is
available, and then runs a working closed-loop throttle over a workload it
*does* own.

The UI reflects hardware reality rather than hiding it. On cards that expose no
settable power limit — common for GeForce and laptop parts, where the vBIOS
owns power behaviour — the Power Limit button is detected as unavailable at
startup and disabled, with the reason printed to the output pane.

## Layout

```
gpu-tool/
  GpuToolbox.cpp      Win32 GUI (no MFC/Qt, no external dependencies)
  GpuCore.{h,cpp}     All logic; NVML bound at runtime from nvml.dll
  CoreTest.cpp        CLI harness over the same code paths
  GpuThrottleDemo.cpp Capability report + closed-loop self-throttle
  build*.bat          MSVC build scripts

*.ps1                 Original PowerShell implementations, kept for
                      differential testing against the C++ ports
GPU-Guide.txt         Full usage guide (Thai)
```

## Building

```
cd gpu-tool
build.bat          -> GpuToolbox.exe
build-test.bat     -> CoreTest.exe
build-demo.bat     -> GpuThrottleDemo.exe
```

No CUDA Toolkit required: `nvml.dll` ships with the NVIDIA driver and is
loaded via `LoadLibrary`/`GetProcAddress`, so no `nvml.h` or import library is
needed. The build scripts point at `E:\Visual Studio`; adjust `VSPATH` if
Visual Studio lives elsewhere.

## CLI

```
CoreTest.exe status              GPU status
CoreTest.exe list                Background processes (read-only)
CoreTest.exe lower | restore     Adjust / restore CPU+IO priority
CoreTest.exe watch <sec> [pct]   Watch mode for a fixed duration

GpuThrottleDemo.exe -t 20 -d 14  Capability report + throttle loop
```

**Note:** the C++ and PowerShell versions store priority state in different
files (`gpu-bgpriority-state.txt` vs `.json`). Lower and restore with the same
implementation — mixing them leaves priorities unrestored.

## Verification

The C++ ports were checked against the PowerShell originals on real hardware:
identical process match counts (48/48), identical save/restore behaviour with
zero mismatches across processes at `AboveNormal`, `High` and `Idle`, and
matching status output including the `[N/A]` power limit.

That last one caught a genuine bug: `nvmlDeviceGetEnforcedPowerLimit` returns
85 W on this card while `nvidia-smi -pl` answers *"not supported in current
scope"*. The capability check now uses `nvmlDeviceGetPowerManagementLimit`,
which correctly reports that no settable limit exists.
