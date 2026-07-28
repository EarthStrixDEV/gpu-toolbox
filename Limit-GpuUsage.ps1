<#
.SYNOPSIS
    Limit / monitor NVIDIA GPU via nvidia-smi.

.DESCRIPTION
    IMPORTANT: GPU utilization percent cannot be capped directly. It is the
    RESULT of the workload submitted, not a knob the driver exposes.

    What CAN actually be capped, and what genuinely controls heat / fan noise
    / power spikes:
      - Power limit (watt cap)  -> nvidia-smi -pl
      - Clock limit  (MHz cap)  -> nvidia-smi -lgc

    Watch mode is a diagnostic aid, not a limiter. It polls utilization and
    logs WHICH process is consuming the GPU when it spikes past the threshold.
    It deliberately kills nothing -- auto-kill risks hitting dwm.exe or an OBS
    session that is mid-recording.

    NOTE ON ENCODING: all runtime strings in this script are ASCII on purpose.
    Windows PowerShell 5.1 reads .ps1 files as ANSI unless they carry a UTF-8
    BOM, so non-ASCII text here turns into mojibake and can break parsing.
    Thai explanation lives in GPU-Scripts-Guide.txt instead.

.PARAMETER Mode
    PowerLimit | ClockLimit | Watch | Status | Reset

.EXAMPLE
    .\Limit-GpuUsage.ps1 -Mode Status
    .\Limit-GpuUsage.ps1 -Mode PowerLimit -PercentOfMax 90
    .\Limit-GpuUsage.ps1 -Mode ClockLimit -MaxClockMHz 2400
    .\Limit-GpuUsage.ps1 -Mode Watch -Threshold 90 -IntervalSec 2
    .\Limit-GpuUsage.ps1 -Mode Reset
#>

[CmdletBinding()]
param(
    [ValidateSet('PowerLimit', 'ClockLimit', 'Watch', 'Status', 'Reset')]
    [string]$Mode = 'Status',

    # GPU index as seen by nvidia-smi (single NVIDIA card = 0).
    # WARNING: this is NOT the same numbering as "GPU 1" in Task Manager.
    [int]$GpuIndex = 0,

    # PowerLimit: percent of the card's max TDP to allow.
    [ValidateRange(30, 100)]
    [int]$PercentOfMax = 90,

    # ClockLimit: graphics clock ceiling in MHz.
    [int]$MaxClockMHz = 2400,

    # Watch: utilization percent considered a "spike".
    # Default 80 rather than 90: at 90 the GPU is already saturated and frame
    # pacing has usually degraded before anything gets logged. 80 catches the
    # ramp while there is still headroom to see what caused it.
    [ValidateRange(1, 100)]
    [int]$Threshold = 80,

    [int]$IntervalSec = 2,

    # Watch: consecutive over-threshold polls required before logging.
    # Filters out harmless momentary spikes.
    [int]$ConsecutiveHits = 3
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# 0) Locate nvidia-smi.exe
#    Usually on PATH, but a minimal driver install may not add it.
# ---------------------------------------------------------------------------
function Get-NvidiaSmi {
    $cmd = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $fallbacks = @(
        "$env:WINDIR\System32\nvidia-smi.exe",
        "$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe"
    )
    foreach ($p in $fallbacks) { if (Test-Path $p) { return $p } }
    return $null
}

$smi = Get-NvidiaSmi
if (-not $smi) {
    Write-Error 'nvidia-smi.exe not found - NVIDIA driver missing, or no NVIDIA GPU present.'
    return
}

# Query nvidia-smi for raw values (no header, no units).
function Invoke-SmiQuery {
    param([string]$Fields)
    $raw = & $smi "--query-gpu=$Fields" '--format=csv,noheader,nounits' "--id=$GpuIndex" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "nvidia-smi query failed: $raw" }
    return ($raw | Select-Object -First 1).ToString().Trim()
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
# Mode: Status - inspect before deciding anything. Read-only, no admin needed.
# ---------------------------------------------------------------------------
if ($Mode -eq 'Status') {
    $f = 'name,utilization.gpu,temperature.gpu,power.draw,power.limit,power.max_limit,clocks.gr'
    $v = (Invoke-SmiQuery -Fields $f) -split '\s*,\s*'

    Write-Host ""
    Write-Host ("GPU {0}          : {1}" -f $GpuIndex, $v[0])
    Write-Host ("Utilization    : {0} %" -f $v[1])
    Write-Host ("Temperature    : {0} C" -f $v[2])
    Write-Host ("Power draw     : {0} W" -f $v[3])
    Write-Host ("Power limit    : {0} W  (max {1} W)" -f $v[4], $v[5])
    Write-Host ("Graphics clock : {0} MHz" -f $v[6])
    Write-Host ""

    Write-Host 'Processes currently using this GPU:'
    & $smi '--query-compute-apps=pid,process_name,used_memory' '--format=csv' "--id=$GpuIndex"
    return
}

# ---------------------------------------------------------------------------
# Mode: Watch - diagnostic only. Touches nothing, no admin needed.
#       Runs until Ctrl+C.
# ---------------------------------------------------------------------------
if ($Mode -eq 'Watch') {
    $logFile = Join-Path $env:LOCALAPPDATA 'gpu-watchdog.log'
    $hits = 0

    Write-Host "Watching GPU $GpuIndex - threshold $Threshold percent, every $IntervalSec s"
    Write-Host "Log: $logFile"
    Write-Host "Press Ctrl+C to stop."
    Write-Host ""

    # --- episode state ------------------------------------------------------
    # An "episode" is one continuous stretch above the threshold. Tracking it
    # as a unit, rather than logging every poll, produces a useful summary at
    # the end (peak / average / duration) instead of near-identical lines.
    $inEpisode  = $false
    $epStart    = $null
    $epPeak     = 0
    $epSum      = 0
    $epSamples  = 0
    $epPeakApps = ''

    # Hysteresis: an open episode only closes once utilization drops a clear
    # margin BELOW the threshold. Without this, a load hovering right at the
    # line opens and closes episodes every few polls and floods the log.
    $clearLevel  = [math]::Max(0, $Threshold - 10)
    $belowStreak = 0

    while ($true) {
        $v    = (Invoke-SmiQuery -Fields 'utilization.gpu,temperature.gpu,power.draw') -split '\s*,\s*'
        $util = [int]$v[0]
        $temp = $v[1]
        $pow  = $v[2]
        $ts   = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

        if ($util -ge $Threshold) {
            $hits++
            $belowStreak = 0

            if (-not $inEpisode -and $hits -ge $ConsecutiveHits) {
                # Sustained load confirmed - open an episode.
                $inEpisode  = $true
                $epStart    = Get-Date
                $epPeak     = 0
                $epSum      = 0
                $epSamples  = 0
                $epPeakApps = ''

                $line = "$ts  EPISODE START  util=$util%  temp=${temp}C  power=${pow}W  (threshold $Threshold%)"
                Add-Content -Path $logFile -Value $line -Encoding UTF8
                Write-Host $line -ForegroundColor Yellow
            }

            if ($inEpisode) {
                $epSum += $util
                $epSamples++

                # Capture the process list at peak load - that sample is the
                # one most likely to name the actual culprit.
                if ($util -gt $epPeak) {
                    $epPeak = $util
                    $apps = (& $smi '--query-compute-apps=pid,process_name,used_memory' `
                                    '--format=csv,noheader' "--id=$GpuIndex") -join ' | '
                    if (-not $apps) { $apps = '(no compute apps - graphics/DWM workload)' }
                    $epPeakApps = $apps
                }
            }
        }
        else {
            $hits = 0

            if ($inEpisode) {
                if ($util -le $clearLevel) { $belowStreak++ } else { $belowStreak = 0 }

                # Two consecutive clearly-below polls end the episode.
                if ($belowStreak -ge 2) {
                    $dur = [int]((Get-Date) - $epStart).TotalSeconds
                    $avg = if ($epSamples -gt 0) { [int]($epSum / $epSamples) } else { 0 }

                    $sum1 = "$ts  EPISODE END    duration=${dur}s  peak=$epPeak%  avg=$avg%"
                    $sum2 = "                 at peak: $epPeakApps"
                    Add-Content -Path $logFile -Value $sum1 -Encoding UTF8
                    Add-Content -Path $logFile -Value $sum2 -Encoding UTF8
                    Write-Host $sum1 -ForegroundColor Green
                    Write-Host $sum2 -ForegroundColor DarkGray

                    $inEpisode   = $false
                    $belowStreak = 0
                }
            }
        }

        Start-Sleep -Seconds $IntervalSec
    }
}

# ---------------------------------------------------------------------------
# Modes below write to the driver -> administrator required.
# ---------------------------------------------------------------------------
if (-not (Test-IsAdmin)) {
    Write-Error 'This mode requires PowerShell started with Run as Administrator.'
    return
}

# Persistence mode helps settings survive when no process is using the GPU.
# Typically unsupported on Windows/GeForce - not a fatal condition.
& $smi '-pm' '1' "--id=$GpuIndex" 2>&1 | Out-Null

switch ($Mode) {

    'PowerLimit' {
        $v      = (Invoke-SmiQuery -Fields 'power.max_limit,power.min_limit') -split '\s*,\s*'
        $maxW   = [double]$v[0]
        $minW   = [double]$v[1]
        $target = [math]::Round($maxW * $PercentOfMax / 100, 0)

        # Clamp to the card's accepted floor, otherwise nvidia-smi rejects the
        # whole command rather than clamping for us.
        if ($target -lt $minW) {
            Write-Warning "$target W is below the minimum ($minW W) - using $minW W instead."
            $target = $minW
        }

        Write-Host "Setting power limit to $target W ($PercentOfMax percent of max $maxW W)"
        $out = & $smi '-pl' $target "--id=$GpuIndex" 2>&1
        Write-Host $out

        if ($LASTEXITCODE -ne 0) {
            Write-Warning @'
Failed to set power limit. Many GeForce cards have -pl locked in the driver
(full support is a Quadro/Tesla feature).
Alternative: use MSI Afterburner's power limit slider instead.
'@
        }
    }

    'ClockLimit' {
        Write-Host "Locking graphics clock to a maximum of $MaxClockMHz MHz"
        $out = & $smi '-lgc' "0,$MaxClockMHz" "--id=$GpuIndex" 2>&1
        Write-Host $out

        if ($LASTEXITCODE -ne 0) {
            Write-Warning 'Failed to set clock limit - card or driver may not support -lgc.'
        }
    }

    'Reset' {
        Write-Host 'Restoring power limit to default...'
        $maxW = [double](Invoke-SmiQuery -Fields 'power.max_limit')
        & $smi '-pl' $maxW "--id=$GpuIndex" 2>&1 | Write-Host

        Write-Host 'Unlocking clocks...'
        & $smi '-rgc' "--id=$GpuIndex" 2>&1 | Write-Host

        Write-Host 'Done.'
    }
}
