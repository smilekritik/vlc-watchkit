param(
    [ValidateSet('Dark', 'Light')][string]$Theme = 'Dark'
)

$ErrorActionPreference = 'Stop'
if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'Close VLC before changing its theme.' }
$directory = Join-Path $env:APPDATA 'vlc'
$preferences = Join-Path $directory 'vlcrc'
$encoding = [Text.UTF8Encoding]::new($false)
$qtSettings = Join-Path $directory 'vlc-qt-interface.ini'
$qtStyle = if ($Theme -eq 'Dark') { 'Fusion' } else { "System's default" }

function Write-ConfigFile([string]$Path, [string]$Text) {
    $temporary = "$Path.vlc-addon-theme.tmp"
    [IO.File]::WriteAllText($temporary, $Text, $encoding)
    [IO.File]::Move($temporary, $Path, $true)
}
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$content = if (Test-Path -LiteralPath $preferences) { [IO.File]::ReadAllText($preferences) } else { "[qt]`r`n" }
$setting = if ($Theme -eq 'Dark') { 'qt-dark-palette=1' } else { 'qt-dark-palette=0' }
$pattern = [regex]::new('(?m)^#?qt-dark-palette=[^\r\n]*')
if ($pattern.IsMatch($content)) {
    $content = $pattern.Replace($content, $setting)
} else {
    $newline = if ($content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $content = $content.TrimEnd() + $newline + $setting + $newline
}
$qt = if (Test-Path -LiteralPath $qtSettings) { [IO.File]::ReadAllText($qtSettings) } else { '' }
$mainWindow = [regex]::new('(?m)^\[MainWindow\][^\r\n]*\r?\n(?:(?!^\[)[\s\S])*')
$group = $mainWindow.Match($qt)
if ($group.Success) {
    $style = [regex]::new('(?m)^QtStyle=[^\r\n]*')
    $updatedGroup = if ($style.IsMatch($group.Value)) {
        $style.Replace($group.Value, "QtStyle=$qtStyle")
    } else {
        $group.Value.Insert($group.Value.IndexOf("`n") + 1, "QtStyle=$qtStyle`n")
    }
    $qt = $qt.Substring(0, $group.Index) + $updatedGroup + $qt.Substring($group.Index + $group.Length)
} else {
    $qt = $qt.TrimEnd() + "`n[MainWindow]`nQtStyle=$qtStyle`n"
}
Write-ConfigFile $qtSettings $qt
Write-ConfigFile $preferences $content
Write-Host "VLC will use the $Theme theme the next time it opens."
