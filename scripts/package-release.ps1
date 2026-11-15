param(
    [Parameter(Mandatory)]
    [ValidatePattern('^v\d+\.\d+\.\d+$')]
    [string]$Tag,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root "build/release/$Tag" }
$out = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$name = "VLC-WatchKit-$Tag-win-x64.zip"
$destination = Join-Path $out $name
$temporary = Join-Path $out ([guid]::NewGuid().ToString('N'))
$sourcePath = "$temporary.source.zip"
$packagePath = "$temporary.release.zip"
$checksumPath = "$temporary.sha256"
$toolchain = Join-Path $root '.deps/llvm-mingw-20250613-msvcrt-x86_64'
$runtime = Join-Path $toolchain 'x86_64-w64-mingw32/share/mingw32'
$assets = [ordered]@{
    'build/libvlc_addon_plugin.dll' = Join-Path $root 'build/libvlc_addon_plugin.dll'
    'licenses/LLVM.txt' = Join-Path $toolchain 'LICENSE.TXT'
    'licenses/COPYING.MinGW-w64.txt' = Join-Path $runtime 'COPYING.MinGW-w64.txt'
    'licenses/COPYING.MinGW-w64-runtime.txt' = Join-Path $runtime 'COPYING.MinGW-w64-runtime.txt'
    'licenses/COPYING.winpthreads.txt' = Join-Path $runtime 'COPYING.winpthreads.txt'
    'licenses/nlohmann-json.txt' = Join-Path $root '.deps/nlohmann-license.txt'
}
foreach ($asset in $assets.Values) {
    if (!(Test-Path -LiteralPath $asset -PathType Leaf)) { throw "Missing $asset. Run scripts/build.ps1 first." }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$level = [IO.Compression.CompressionLevel]::Optimal
$date = git -C $root log -1 --format=%cI "refs/tags/$Tag"
if ($LASTEXITCODE) { throw "Git tag $Tag is required." }
$timestamp = [DateTimeOffset]::Parse($date).ToUniversalTime()

function Write-ZipEntry($Path, [IO.Stream]$InputFile) {
    try {
        $entry = $package.CreateEntry($Path, $level)
        $entry.LastWriteTime = $timestamp
        $outputFile = $entry.Open()
        try { $InputFile.CopyTo($outputFile) } finally { $outputFile.Dispose() }
    } finally { $InputFile.Dispose() }
}

try {
    git -C $root archive --format=zip "--output=$sourcePath" "refs/tags/$Tag"
    if ($LASTEXITCODE) { throw "Could not archive $Tag." }
    $source = [IO.Compression.ZipFile]::OpenRead($sourcePath)
    try {
        $package = [IO.Compression.ZipFile]::Open($packagePath, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in $source.Entries) {
                if (!$file.Name) { continue }
                Write-ZipEntry $file.FullName ($file.Open())
            }
            foreach ($asset in $assets.GetEnumerator()) {
                Write-ZipEntry $asset.Key ([IO.File]::OpenRead($asset.Value))
            }
        } finally { $package.Dispose() }
    } finally { $source.Dispose() }

    $package = [IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $package.Entries) {
            $path = $entry.FullName
            if ($path -match '(^|/)\.{1,2}(/|$)|^/|//|/$|[\\:]' -or !$paths.Add($path)) {
                throw "Unexpected or duplicate ZIP path: $path"
            }
        }
        foreach ($required in @('Install WatchKit.bat', 'scripts/install.ps1', 'README.md', 'lua/vlc_addon_settings.lua', 'lua/vlc_addon_bookmarks.lua', 'lua/vlc_addon_continue.lua') + @($assets.Keys)) {
            if (!$package.GetEntry($required)) { throw "ZIP entry missing: $required" }
        }
    } finally { $package.Dispose() }

    $hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name" | Set-Content -LiteralPath $checksumPath -Encoding ascii
    Move-Item -LiteralPath $packagePath -Destination $destination -Force
    Move-Item -LiteralPath $checksumPath -Destination (Join-Path $out 'SHA256SUMS.txt') -Force
    Write-Host "Verified $destination"
} finally {
    foreach ($path in $sourcePath, $packagePath, $checksumPath) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
}
