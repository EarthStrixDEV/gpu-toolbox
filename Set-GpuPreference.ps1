<#
.SYNOPSIS
    Force a per-app GPU preference for dwm.exe / obs.exe via Windows Graphics
    Settings.

.DESCRIPTION
    Windows stores per-app GPU preference at
        HKCU\Software\Microsoft\DirectX\UserGpuPreferences
    where the value NAME is the full path to the .exe and the value DATA is
    "GpuPreference=N;"
        N = 0  -> Let Windows decide (default)
        N = 1  -> Power saving GPU   (usually the iGPU)
        N = 2  -> High performance   (usually the NVIDIA dGPU)

    Why two modes exist:
      - Goal is "get this process OFF the NVIDIA card because it peaks 100%"
        -> use -Mode PowerSaving
      - Goal is "force it ONTO the NVIDIA card for performance"
        -> use -Mode HighPerformance

    The value is read when a process first creates its D3D device, so it only
    takes effect on restart. -Restart closes OBS for you. dwm.exe is never
    killed: terminating DWM breaks the desktop session.

    NOTE ON ENCODING: every runtime string here is ASCII on purpose. Windows
    PowerShell 5.1 reads .ps1 files as ANSI unless they carry a UTF-8 BOM, so
    Thai text in this file turns into mojibake and breaks parsing. Thai
    explanation lives in GPU-Scripts-Guide.txt instead.

.PARAMETER Mode
    PowerSaving | HighPerformance | Reset

.PARAMETER Restart
    Close obs.exe after applying, so the new preference takes effect.

.PARAMETER ObsPath
    Full path to obs64.exe. Use this when OBS lives somewhere the auto-detect
    does not cover (a Steam library on another drive, a portable install).
    When omitted the script detects OBS itself: running process first, then
    the known install locations.

.PARAMETER SkipDwm
    Do not touch the dwm.exe entry. Useful because the DWM preference rarely
    has any real effect but is easy to clobber accidentally.

.EXAMPLE
    .\Set-GpuPreference.ps1 -Mode PowerSaving -Restart
    .\Set-GpuPreference.ps1 -Mode HighPerformance
    .\Set-GpuPreference.ps1 -Mode Reset
    .\Set-GpuPreference.ps1 -Mode PowerSaving -ObsPath 'E:\Games\OBS\bin\64bit\obs64.exe'
#>

[CmdletBinding()]
param(
    [ValidateSet('PowerSaving', 'HighPerformance', 'Reset')]
    [string]$Mode = 'PowerSaving',

    [switch]$Restart,

    # Explicit OBS location; overrides auto-detection entirely.
    [string]$ObsPath,

    [switch]$SkipDwm
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# 1) Targets
#    The registry key name must be a FULL path - a bare exe name has no
#    effect. dwm.exe is always in System32; OBS is located from the running
#    process first (most accurate), then from standard install paths.
# ---------------------------------------------------------------------------
$RegPath = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'

function Resolve-TargetPath {
    param(
        [string]$ProcessName,        # process name without .exe
        [string[]]$FallbackPaths     # used when the process is not running
    )

    $running = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
               Select-Object -First 1
    if ($running -and $running.Path) { return $running.Path }

    foreach ($p in $FallbackPaths) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$targets = @()

if (-not $SkipDwm) {
    $dwm = Resolve-TargetPath -ProcessName 'dwm' -FallbackPaths @(
        "$env:WINDIR\System32\dwm.exe"
    )
    if ($dwm) { $targets += [pscustomobject]@{ Name = 'dwm.exe'; Path = $dwm } }
}

# -ObsPath wins outright. Validate it up front rather than silently writing a
# registry entry for a file that does not exist - a bad path would produce a
# preference Windows can never match against a real process.
if ($ObsPath) {
    if (-not (Test-Path -LiteralPath $ObsPath -PathType Leaf)) {
        Write-Error "-ObsPath does not point at an existing file: $ObsPath"
        return
    }
    $obs = (Resolve-Path -LiteralPath $ObsPath).Path
    Write-Host ("[obs  ]  using -ObsPath override")
} else {
    $obs = Resolve-TargetPath -ProcessName 'obs64' -FallbackPaths @(
        'C:\Program Files\obs-studio\bin\64bit\obs64.exe',
        'C:\Program Files (x86)\obs-studio\bin\64bit\obs64.exe',
        "$env:LOCALAPPDATA\Programs\obs-studio\bin\64bit\obs64.exe",
        # Steam installs OBS under the library folder, which may be on any drive.
        'C:\Program Files (x86)\Steam\steamapps\common\OBS Studio\bin\64bit\obs64.exe',
        'D:\SteamLibrary\steamapps\common\OBS Studio\bin\64bit\obs64.exe',
        'E:\SteamLibrary\steamapps\common\OBS Studio\bin\64bit\obs64.exe'
    )
    if (-not $obs) {
        $obs = Resolve-TargetPath -ProcessName 'obs' -FallbackPaths @()
    }
}

if ($obs) { $targets += [pscustomobject]@{ Name = 'obs.exe'; Path = $obs } }

if ($targets.Count -eq 0) {
    Write-Warning 'No target process found - nothing to do.'
    Write-Warning 'If OBS is installed somewhere unusual, pass -ObsPath explicitly.'
    return
}

if (-not $obs) {
    Write-Warning 'OBS was not found. Pass -ObsPath to point at obs64.exe directly.'
}

# ---------------------------------------------------------------------------
# 2) Write the registry values
# ---------------------------------------------------------------------------
if (-not (Test-Path $RegPath)) {
    New-Item -Path $RegPath -Force | Out-Null
}

$prefValue = switch ($Mode) {
    'PowerSaving'     { 'GpuPreference=1;' }
    'HighPerformance' { 'GpuPreference=2;' }
    'Reset'           { $null }
}

foreach ($t in $targets) {
    if ($Mode -eq 'Reset') {
        # Removing the value returns the app to "let Windows decide".
        try {
            Remove-ItemProperty -Path $RegPath -Name $t.Path -ErrorAction Stop
            Write-Host ("[reset]  {0}" -f $t.Path)
        } catch {
            Write-Host ("[skip ]  {0} (no preference was set)" -f $t.Path)
        }
        continue
    }

    New-ItemProperty -Path  $RegPath `
                     -Name  $t.Path `
                     -Value $prefValue `
                     -PropertyType String `
                     -Force | Out-Null

    Write-Host ("[set  ]  {0}" -f $t.Path)
    Write-Host ("         -> {0}" -f $prefValue)
}

# ---------------------------------------------------------------------------
# 3) Restart OBS only
#    GpuPreference is read when the process first creates its D3D device, so
#    a running instance keeps using the old GPU until restarted.
# ---------------------------------------------------------------------------
if ($Restart) {
    $obsProcs = Get-Process -Name 'obs64', 'obs' -ErrorAction SilentlyContinue
    if ($obsProcs) {
        Write-Host ""
        Write-Host 'Closing OBS (any recording or stream in progress will stop)...'
        $obsProcs | Stop-Process -Force
        Write-Host 'Closed - reopen OBS for the new preference to take effect.'
    } else {
        Write-Host ""
        Write-Host 'OBS is not running - it will pick up the new value next launch.'
    }
}

# ---------------------------------------------------------------------------
# 4) Caveats worth repeating at the point of use
# ---------------------------------------------------------------------------
if ($Mode -ne 'Reset') {
    Write-Host ""
    Write-Host '--- Notes ---'
    Write-Host 'dwm.exe : this setting usually has no real effect. DWM must render on'
    Write-Host '          the GPU that drives the display. If your monitor cable goes'
    Write-Host '          straight into the NVIDIA card, DWM stays on NVIDIA no matter'
    Write-Host '          what. The fix that works is moving the cable to the'
    Write-Host '          motherboard video port (iGPU), and disabling hardware'
    Write-Host '          acceleration in GPU-heavy apps (Chrome, Discord, etc).'
    Write-Host 'obs.exe : on Power Saving the NVENC encoder becomes unavailable.'
    Write-Host '          Switch to x264 (CPU) or QuickSync (Intel iGPU) under'
    Write-Host '          Settings > Output first.'
}
