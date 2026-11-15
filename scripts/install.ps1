param(
    [string]$VlcDirectory,
    [string]$ExpectedUserSid
)

$ErrorActionPreference = 'Stop'
$releaseUrl = 'https://github.com/smilekritik/vlc-watchkit/releases/latest'
$vlcUrl = 'https://download.videolan.org/vlc/3.0.24/win64/vlc-3.0.24-win64.exe'
$root = Split-Path -Parent $PSScriptRoot
$dataDirectory = Join-Path $env:LOCALAPPDATA 'VLCAddon'
$receipt = Join-Path $dataDirectory 'installation.json'
$dll = Join-Path $root 'build/libvlc_addon_plugin.dll'
$nextStep = "Download the Windows x64 ZIP under Assets, then use Extract All. $releaseUrl"
$snapshots = $null

try {
    if (!(Test-Path -LiteralPath $dll -PathType Leaf)) { throw 'The release DLL is missing.' }
    $menus = foreach ($name in 'settings', 'bookmarks', 'continue') {
        Get-Item -LiteralPath (Join-Path $root "lua/vlc_addon_$name.lua")
    }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ($ExpectedUserSid -and $identity.User.Value -ne $ExpectedUserSid) {
        $nextStep = 'Sign in to the Windows account that will use VLC and approve setup from that account.'
        throw 'Windows started the installer under a different account. VLC preferences are per user.'
    }

    $nextStep = 'Close every VLC window and run setup again. If VLC is still running, exit it from the notification area.'
    if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'VLC is still running.' }

    $nextStep = "Check that you have VLC 3.0.24 x64 installed. $vlcUrl"
    $history = if (Test-Path -LiteralPath $receipt) {
        Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    }
    if (!$VlcDirectory) {
        $registered = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\VideoLAN\VLC' -ErrorAction SilentlyContinue
        $candidates = @($history.vlc, $registered.InstallDir, (Join-Path $env:ProgramFiles 'VideoLAN\VLC'))
        $VlcDirectory = $candidates | Where-Object {
            $_ -and (Test-Path -LiteralPath (Join-Path $_ 'vlc.exe') -PathType Leaf)
        } | Select-Object -First 1
    }
    if (!$VlcDirectory -or !(Test-Path -LiteralPath (Join-Path $VlcDirectory 'vlc.exe') -PathType Leaf)) {
        $nextStep += ' For a custom folder, run Install WatchKit.bat -VlcDirectory "D:\Apps\VLC".'
        throw 'Setup could not find vlc.exe.'
    }
    $VlcDirectory = (Get-Item -LiteralPath $VlcDirectory).FullName.TrimEnd('\')
    if ($history.vlc -and [IO.Path]::GetFullPath($history.vlc).TrimEnd('\') -ne $VlcDirectory) {
        $nextStep = "Uninstall WatchKit from $($history.vlc) before installing it in another VLC folder."
        throw 'WatchKit is already registered for a different VLC installation.'
    }
    $player = Get-Item -LiteralPath (Join-Path $VlcDirectory 'vlc.exe')
    $version = $player.VersionInfo.ProductVersion
    if ($version -notmatch '^3[.,]0[.,]24(?:[.,]|$)') { throw "Found VLC version '$version' at $VlcDirectory." }
    $bytes = [IO.File]::ReadAllBytes($player.FullName)
    if ([BitConverter]::ToUInt16($bytes, [BitConverter]::ToInt32($bytes, 60) + 4) -ne 0x8664) {
        throw "Found a VLC executable that is not x64 at $VlcDirectory."
    }

    $pluginDirectory = Join-Path $VlcDirectory 'plugins'
    $plugin = Join-Path $pluginDirectory 'control/libvlc_addon_plugin.dll'
    $cacheGenerator = Join-Path $VlcDirectory 'vlc-cache-gen.exe'
    if (!(Test-Path -LiteralPath (Split-Path -Parent $plugin) -PathType Container) -or
        !(Test-Path -LiteralPath $cacheGenerator -PathType Leaf)) {
        throw "The VLC plugin directory or cache tool is missing from $VlcDirectory."
    }

    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $nextStep = 'Run the installer again and approve the Windows administrator prompt.'
        $powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}" -VlcDirectory "{1}" -ExpectedUserSid {2}' -f
            $PSCommandPath, $VlcDirectory, $identity.User.Value
        Write-Host 'Approve the Windows prompt so setup can install the VLC plugin.'
        $setup = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        exit $setup.ExitCode
    }

    Write-Host "Installing WatchKit for VLC in $VlcDirectory"
    $configDirectory = Join-Path $env:APPDATA 'vlc'
    $preferences = Join-Path $configDirectory 'vlcrc'
    $libraries = Join-Path $configDirectory 'lua/extensions'
    $backup = Join-Path $dataDirectory 'vlcrc.before-install'
    $cache = Join-Path $pluginDirectory 'plugins.dat'
    $nextStep = 'Check that your Windows account can write to its VLC and WatchKit settings folders.'
    New-Item -ItemType Directory -Path $dataDirectory,$libraries -Force | Out-Null
    $encoding = [Text.UTF8Encoding]::new($false)
    $content = if (Test-Path -LiteralPath $preferences) { [IO.File]::ReadAllText($preferences) } else { "[core]`r`n" }
    $changes = [ordered]@{'extraintf' = ''; 'one-instance' = '1'; 'playlist-enqueue' = '1'; 'qt-continue' = '0'}
    $prior = [ordered]@{}
    $activeSettings = @{}
    foreach ($name in $changes.Keys) {
        $pattern = '(?m)^' + [regex]::Escape($name) + '=([^\r\n]*)'
        $activeSettings[$name] = [regex]::new($pattern, [Text.RegularExpressions.RegexOptions]::RightToLeft)
        $match = $activeSettings[$name].Match($content)
        $prior[$name] = if ($match.Success) { $match.Groups[1].Value } else { $null }
    }
    $extras = @($prior['extraintf'] -split ':' | Where-Object { $_ -and $_ -ne 'vlc_addon' })
    $changes['extraintf'] = ($extras + @('vlc_addon')) -join ':'
    foreach ($name in $changes.Keys) {
        $replacement = "$name=$($changes[$name])"
        $setting = $activeSettings[$name]
        $effective = $setting.Match($content)
        if ($effective.Success) {
            # VLC reads active settings in file order. Keep earlier values as comments.
            $content = $setting.Replace($content, {
                param($match)
                if ($match.Index -eq $effective.Index) { $replacement }
                else { '#' + $match.Value }
            })
            continue
        }
        $comment = [regex]::new('(?m)^#' + [regex]::Escape($name) + '=[^\r\n]*')
        if ($comment.IsMatch($content)) { $content = $comment.Replace($content, { param($match) $replacement }, 1) }
        else { $content = $content.TrimEnd() + "`r`n$replacement`r`n" }
    }

    $paths = @($plugin, $cache, $receipt, $backup, $preferences) + @($menus | ForEach-Object {
        Join-Path $libraries $_.Name
    })
    $originals = [ordered]@{}
    foreach ($path in $paths) {
        $originals[$path] = $null
        if (Test-Path -LiteralPath $path -PathType Leaf) { $originals[$path] = [IO.File]::ReadAllBytes($path) }
    }
    $snapshots = $originals
    $nextStep = 'Close VLC, check that the VLC folder is writable, and retry. Repair VLC if its cache tool keeps failing.'
    Copy-Item -LiteralPath $dll -Destination $plugin -Force
    & $cacheGenerator $pluginDirectory
    if ($LASTEXITCODE) { throw "VLC's plugin cache tool returned exit code $LASTEXITCODE." }

    $nextStep = 'Check that your Windows account can write to its VLC and WatchKit settings folders, then retry.'
    if (!$history) {
        if (Test-Path -LiteralPath $preferences) { Copy-Item -LiteralPath $preferences -Destination $backup }
        @{ version = 1; vlc = $VlcDirectory; original = $prior } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    }
    foreach ($menu in $menus) { Copy-Item -LiteralPath $menu.FullName -Destination $libraries -Force }
    [IO.File]::WriteAllText($preferences, $content, $encoding)
    Write-Host 'WatchKit is installed. Launch VLC and open View > WatchKit Settings.'
} catch {
    Write-Host 'Could not install WatchKit for VLC.' -ForegroundColor Red
    Write-Host $_.Exception.Message
    Write-Host $nextStep
    if ($snapshots) {
        $restored = $true
        foreach ($path in $snapshots.Keys) {
            try {
                if ($null -ne $snapshots[$path]) { [IO.File]::WriteAllBytes($path, $snapshots[$path]) }
                elseif (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
            } catch {
                $restored = $false
                Write-Host "Could not restore $path. Windows reported: $($_.Exception.Message)"
            }
        }
        if ($restored) { Write-Host 'Setup restored the previous VLC files.' }
    }
    if ($ExpectedUserSid) { $null = Read-Host 'Press Enter to return to setup' }
    exit 1
}
