<#
.SYNOPSIS
    Lower CPU and I/O priority of background processes so the foreground
    workload (game / OBS encode) gets scheduled first.

.DESCRIPTION
    WHAT THIS DOES AND DOES NOT DO - read this before expecting a GPU drop.

    This script lowers CPU priority class and I/O priority. It does NOT limit
    GPU usage. Windows exposes no per-process GPU quota: GPU scheduling is
    owned by the driver and the hardware, and there is no supported API to say
    "this process may use at most N percent of the GPU". Anything claiming
    otherwise is either changing CPU priority (this script) or changing which
    adapter an app runs on (Set-GpuPreference.ps1).

    What lowering CPU priority actually buys you: when the CPU is contended,
    the game and the OBS encoder thread get scheduled ahead of browsers,
    updaters, and overlays. That reduces frame-time spikes caused by CPU
    starvation. If your bottleneck is purely the GPU, expect little to no
    change - and the honest answer is that this script will not help.

    Priority is a per-process runtime attribute: it resets when the process
    restarts, and nothing here is written to disk or the registry.

    NOTE ON ENCODING: runtime strings are ASCII on purpose. Windows PowerShell
    5.1 reads .ps1 files as ANSI unless they carry a UTF-8 BOM, so non-ASCII
    text here would become mojibake and break parsing.

.PARAMETER Mode
    Lower   - drop matched background processes to BelowNormal + low I/O
    Restore - put matched processes back to Normal priority
    List    - show what would be matched, change nothing

.PARAMETER Names
    Process names (no .exe) to treat as background. Defaults cover the usual
    GPU/CPU-hungry background offenders seen on this machine.

.PARAMETER Aggressive
    Use Idle priority instead of BelowNormal. Idle means the process only runs
    when nothing else wants the CPU, which can make browsers visibly stutter.

.EXAMPLE
    .\Set-BackgroundPriority.ps1 -Mode List
    .\Set-BackgroundPriority.ps1 -Mode Lower
    .\Set-BackgroundPriority.ps1 -Mode Restore
#>

[CmdletBinding()]
param(
    [ValidateSet('Lower', 'Restore', 'List')]
    [string]$Mode = 'List',

    [string[]]$Names = @(
        'firefox',
        'msedgewebview2',
        'chrome',
        'brave',
        'Discord',
        'NVIDIA Overlay',
        'NVIDIA Web Helper',
        'EPConsole',
        'SearchHost',
        'CrossDeviceResume',
        'TextInputHost',
        'OneDrive',
        'Widgets',
        'WidgetService'
    ),

    [switch]$Aggressive
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Never touch these. Lowering priority on a critical system process degrades
# the whole desktop, and obs64/the game are the things we are protecting.
# ---------------------------------------------------------------------------
$Protected = @(
    'dwm', 'csrss', 'winlogon', 'services', 'lsass', 'smss', 'wininit',
    'System', 'Idle', 'Registry', 'obs64', 'obs', 'powershell', 'pwsh',
    'WindowsTerminal', 'GpuToolbox'
)

$targetPriority = if ($Aggressive) { 'Idle' } else { 'BelowNormal' }

# ---------------------------------------------------------------------------
# I/O priority needs NtSetInformationProcess - not exposed by .NET, so it is
# P/Invoked here. ProcessIoPriority is information class 33; value 0 = Very
# Low, 2 = Normal. This is undocumented-but-stable Windows behaviour, so every
# call is wrapped and failure is non-fatal.
# ---------------------------------------------------------------------------
if (-not ('NativeIo' -as [type])) {
    Add-Type -Namespace '' -Name NativeIo -MemberDefinition @'
[DllImport("ntdll.dll", SetLastError = true)]
public static extern int NtSetInformationProcess(
    IntPtr hProcess, int cls, ref int value, int length);
'@
}

function Set-IoPriority {
    param([System.Diagnostics.Process]$Proc, [int]$Value)
    try {
        $v = $Value
        # 33 = ProcessIoPriority
        $rc = [NativeIo]::NtSetInformationProcess($Proc.Handle, 33, [ref]$v, 4)
        return ($rc -eq 0)
    } catch {
        return $false
    }
}

# ---------------------------------------------------------------------------
# Collect matching processes
# ---------------------------------------------------------------------------
$matched = @()
foreach ($n in $Names) {
    $procs = Get-Process -Name $n -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($Protected -contains $p.ProcessName) { continue }
        $matched += $p
    }
}

if ($matched.Count -eq 0) {
    Write-Host 'No matching background processes are running.'
    return
}

# ---------------------------------------------------------------------------
# List mode - report only
# ---------------------------------------------------------------------------
if ($Mode -eq 'List') {
    Write-Host ''
    Write-Host ('{0,-24} {1,-8} {2,-14} {3}' -f 'PROCESS', 'PID', 'PRIORITY', 'CPU(s)')
    Write-Host ('-' * 62)
    foreach ($p in ($matched | Sort-Object ProcessName)) {
        $cpu = try { [math]::Round($p.TotalProcessorTime.TotalSeconds, 1) } catch { 'n/a' }
        $pri = try { $p.PriorityClass } catch { 'access denied' }
        Write-Host ('{0,-24} {1,-8} {2,-14} {3}' -f $p.ProcessName, $p.Id, $pri, $cpu)
    }
    Write-Host ''
    Write-Host ("Total matched: {0}" -f $matched.Count)
    Write-Host 'Run with -Mode Lower to apply, -Mode Restore to undo.'
    return
}

# ---------------------------------------------------------------------------
# Save / restore of ORIGINAL priorities
#
# Processes do not all start at Normal - browsers routinely run helper
# processes at AboveNormal or Idle, and TextInputHost sits at High. Restoring
# everything to a flat "Normal" would silently overwrite those values instead
# of restoring them, so Lower records what each PID was before it changed it.
#
# Keyed by PID, which is only meaningful while the process lives - that is
# fine, because priority itself resets when a process restarts.
# ---------------------------------------------------------------------------
$stateFile = Join-Path $env:LOCALAPPDATA 'gpu-bgpriority-state.json'

$okCount = 0
$failed  = @()

if ($Mode -eq 'Lower') {
    $saved = @{}

    foreach ($p in $matched) {
        try {
            # Read the current value BEFORE changing it.
            $orig = $p.PriorityClass.ToString()

            # Already at or below target - leave it alone so Restore cannot
            # promote a process that was deliberately idle to begin with.
            if ($orig -eq 'Idle' -or ($orig -eq 'BelowNormal' -and -not $Aggressive)) {
                Write-Host ("[skip    ] {0} (pid {1}) already {2}" -f $p.ProcessName, $p.Id, $orig)
                continue
            }

            $p.PriorityClass = $targetPriority
            $ioOk = Set-IoPriority -Proc $p -Value 0
            $tag  = if ($ioOk) { 'cpu+io' } else { 'cpu only' }

            $saved["$($p.Id)"] = @{ Name = $p.ProcessName; Priority = $orig }

            Write-Host ("[{0,-8}] {1} (pid {2}) {3} -> {4}" -f `
                        $tag, $p.ProcessName, $p.Id, $orig, $targetPriority)
            $okCount++
        }
        catch {
            # Elevated or protected processes reject the change - expected.
            $failed += "$($p.ProcessName) (pid $($p.Id))"
        }
    }

    if ($saved.Count -gt 0) {
        $saved | ConvertTo-Json -Depth 4 | Set-Content -Path $stateFile -Encoding UTF8
        Write-Host ''
        Write-Host "Saved original priorities to: $stateFile"
    }
}
else {
    # --- Restore ---------------------------------------------------------
    if (-not (Test-Path $stateFile)) {
        Write-Warning 'No saved state found - nothing was lowered by this script.'
        Write-Warning "Expected: $stateFile"
        Write-Warning 'Refusing to blanket-set everything to Normal, which would'
        Write-Warning 'overwrite priorities this script never changed.'
        return
    }

    $saved = Get-Content $stateFile -Raw | ConvertFrom-Json

    foreach ($entry in $saved.PSObject.Properties) {
        $procId = [int]$entry.Name
        $orig   = $entry.Value.Priority
        $name   = $entry.Value.Name

        $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if (-not $p) {
            Write-Host ("[gone    ] {0} (pid {1}) - restarted since, priority already reset" -f $name, $procId)
            continue
        }
        if ($p.ProcessName -ne $name) {
            # PID was recycled onto a different process - do not touch it.
            Write-Host ("[skip    ] pid {0} is now '{1}', not '{2}'" -f $procId, $p.ProcessName, $name)
            continue
        }

        try {
            $p.PriorityClass = $orig
            $ioOk = Set-IoPriority -Proc $p -Value 2
            $tag  = if ($ioOk) { 'cpu+io' } else { 'cpu only' }
            Write-Host ("[{0,-8}] {1} (pid {2}) -> {3}" -f $tag, $name, $procId, $orig)
            $okCount++
        }
        catch {
            $failed += "$name (pid $procId)"
        }
    }

    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host ("Changed: {0}" -f $okCount)

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host 'Access denied (run as Administrator to include these):'
    foreach ($f in $failed) { Write-Host "  $f" }
}

if ($Mode -eq 'Lower') {
    Write-Host ''
    Write-Host '--- Reality check ---'
    Write-Host 'This changed CPU/IO priority only. It does not cap GPU usage -'
    Write-Host 'Windows has no per-process GPU quota API. If your bottleneck is'
    Write-Host 'the GPU itself, this will not move the needle. Priority also'
    Write-Host 'resets when each process restarts.'
}
