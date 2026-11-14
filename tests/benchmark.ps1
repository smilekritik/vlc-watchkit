param(
    [string]$VlcDirectory = "$env:ProgramFiles\VideoLAN\VLC",
    [int]$Runs = 3,
    [int]$SampleSeconds = 5
)
$ErrorActionPreference = 'Stop'
if ($Runs -lt 1 -or $SampleSeconds -lt 1) { throw 'Runs and SampleSeconds must be positive.' }
if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'Close VLC before benchmarking.' }
. "$PSScriptRoot/window-tools.ps1"
$root = Split-Path -Parent $PSScriptRoot
$beforePlugin = $env:VLC_PLUGIN_PATH
$beforeData = $env:VLC_ADDON_DATA_DIR
$env:VLC_PLUGIN_PATH = Join-Path $root 'build'
$env:VLC_ADDON_DATA_DIR = Join-Path $root ('build/benchmark-' + [guid]::NewGuid().ToString('N'))
$player = Join-Path $VlcDirectory 'vlc.exe'

function Measure-Player([bool]$Addon) {
    $arguments = '--ignore-config --no-one-instance --no-media-library --no-qt-privacy-ask'
    if ($Addon) { $arguments += ' --extraintf=vlc_addon' }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process $player -ArgumentList $arguments -PassThru
    try {
        do {
            $process.Refresh()
            $window = $process.MainWindowHandle
            if ($process.HasExited -or $watch.Elapsed.TotalSeconds -ge 10) { throw 'VLC window did not open.' }
            if (!$window) { Start-Sleep -Milliseconds 20 }
        } until ($window)
        $startup = $watch.Elapsed.TotalMilliseconds
        Start-Sleep -Seconds 1
        $process.Refresh()
        $cpu = $process.TotalProcessorTime.TotalMilliseconds
        Start-Sleep -Seconds $SampleSeconds
        $process.Refresh()
        [pscustomobject]@{
            Mode = if ($Addon) { 'Addon' } else { 'Baseline' }
            StartupMs = [math]::Round($startup, 1)
            IdleCpuMs = [math]::Round($process.TotalProcessorTime.TotalMilliseconds - $cpu, 1)
            PrivateMiB = [math]::Round($process.PrivateMemorySize64 / 1MB, 2)
            WorkingSetMiB = [math]::Round($process.WorkingSet64 / 1MB, 2)
        }
    } finally {
        if (!$process.HasExited -and (!$process.CloseMainWindow() -or !$process.WaitForExit(5000))) {
            Stop-Process -Id $process.Id
        }
    }
}

try {
    foreach ($index in 1..$Runs) {
        Measure-Player $false
        Measure-Player $true
    }
} finally {
    $env:VLC_PLUGIN_PATH = $beforePlugin
    $env:VLC_ADDON_DATA_DIR = $beforeData
}
