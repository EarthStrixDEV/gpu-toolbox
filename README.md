# GPU Toolbox

**English** · [ภาษาไทย](README.th.md)

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

The UI reflects hardware reality rather than hiding it. Each hardware lever is
probed independently at startup and disabled if the driver refuses it, with the
reason printed to the output pane.

The two levers are not interchangeable, and testing only one would be
misleading. On the reference RTX 3060 Laptop GPU:

| Lever | Result |
|---|---|
| Power limit (`nvidia-smi -pl`) | Refused — *"not supported in current scope"*, even elevated |
| Graphics clock lock (`nvidia-smi -lgc`) | **Works** |

So on that hardware the clock lock is the only usable way to hold down heat and
power draw. `ClockLimitSupported()` determines this by attempting a real no-op
lock at the card's own maximum and checking whether the driver accepts it,
rather than inferring from the model name.

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

## Elevation

`GpuToolbox.exe` requires administrator rights. `GpuToolbox.manifest` requests
`requireAdministrator` and is embedded into the binary at link time, so Windows
shows the UAC prompt before the process starts and refuses to launch it if
consent is denied — a runtime token check cannot elevate an already-running
process.

Practical consequences:

- The exe cannot start silently from Explorer, the Startup folder, or a plain
  Task Scheduler entry. For unattended auto-start, create a scheduled task with
  *Run with highest privileges* enabled.
- `/MANIFESTUAC:NO` in `build.bat` is mandatory, not cosmetic. Without it the
  linker emits its own `asInvoker` trustInfo block, which collides with ours
  and fails the build with `manifest authoring error c1010001`.
- `CoreTest.exe` is unaffected and still runs unelevated, which is why it
  remains the better choice for read-only checks like `status` and `list`.

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
