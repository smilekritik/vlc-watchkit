param(
    [string]$VlcDirectory = "$env:ProgramFiles\VideoLAN\VLC",
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    function Invoke-Native($Executable, [string[]]$Arguments) {
        & $Executable @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
    }
    function Get-Dependency($Path, $Uri, $Sha256) {
        if (Test-Path -LiteralPath $Path) {
            if ((Get-FileHash -LiteralPath $Path).Hash -eq $Sha256) { return }
            throw "Checksum mismatch for $Path. Remove the file and rerun the build."
        }
        $partial = "$Path.partial"
        Invoke-Native curl.exe @('--fail', '--location', '--silent', '--show-error', $Uri, '-o', $partial)
        if ((Get-FileHash -LiteralPath $partial).Hash -ne $Sha256) { throw "Checksum mismatch for $Uri" }
        Move-Item -LiteralPath $partial -Destination $Path -Force
    }
    $version = '3.0.24'
    $toolchain = 'llvm-mingw-20250613-msvcrt-x86_64'
    New-Item -ItemType Directory -Force -Path .deps/json,build | Out-Null
    $player = Get-Item -LiteralPath (Join-Path $VlcDirectory 'vlc.exe')
    if ($player.VersionInfo.ProductVersion -notmatch '^3[.,]0[.,]24(?:[.,]|$)') {
        throw 'Use the x64 VLC 3.0.24 distribution to build the plugin.'
    }
    $core = Join-Path $VlcDirectory 'libvlccore.dll'
    Get-Dependency ".deps/$toolchain.zip" "https://github.com/mstorsjo/llvm-mingw/releases/download/20250613/$toolchain.zip" F2013506C56F3DC9C48D6A1229A1099EB9056A929E5FF79ECC574AFEA0EB75C4
    if (!(Test-Path ".deps/$toolchain/bin/clang++.exe")) {
        Invoke-Native tar.exe @('-xf', ".deps/$toolchain.zip", '-C', '.deps')
    }
    Get-Dependency '.deps/vlc.tar.gz' "https://codeload.github.com/videolan/vlc/tar.gz/refs/tags/$version" 0BA29DF0211E6FC826654EDA3CECE53A5D916D45198F137FC8831BB77AAE2F76
    if (!(Test-Path ".deps/vlc-$version/include/vlc_plugin.h")) {
        Invoke-Native tar.exe @('-xf', '.deps/vlc.tar.gz', '-C', '.deps', "vlc-$version/include")
    }
    Get-Dependency '.deps/json/json.hpp' 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63
    Get-Dependency '.deps/nlohmann-license.txt' 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT' 46A65CFFD1EA955132D95A8DD921640714A8D6B537D2E4E482D31145AE95B603
    $bin = ".deps/$toolchain/bin"
    $exports = Invoke-Native "$bin/llvm-readobj.exe" @('--file-headers', '--coff-exports', $core)
    if (-not ($exports -match 'IMAGE_FILE_MACHINE_AMD64')) { throw 'VLC must be x64.' }
    $symbols = $exports | ForEach-Object {
        if ($_ -match '^\s+Name: ([A-Za-z_][A-Za-z_0-9]*)$') { $Matches[1] }
    }
    @('LIBRARY libvlccore.dll', 'EXPORTS') + $symbols | Set-Content -Encoding ascii build/libvlccore.def
    Invoke-Native "$bin/llvm-dlltool.exe" @('-m', 'i386:x86-64', '-d', 'build/libvlccore.def', '-l', 'build/libvlccore.a')
    $flags = @('-std=c++20', '-O2', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
        '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
        '-ffunction-sections', '-fdata-sections')
    $sources = @(Get-ChildItem src -Filter *.cpp | ForEach-Object FullName)
    Invoke-Native "$bin/clang++.exe" ($flags + @('-D__PLUGIN__', '-isystem', ".deps/vlc-$version/include",
        '-isystem', '.deps/json', '-shared', '-static', '-Wl,--gc-sections', '-Wl,--strip-all', '-Wl,--no-insert-timestamp',
        '-o', 'build/libvlc_addon_plugin.dll') + $sources + @('build/libvlccore.a', '-lcomctl32', '-lshell32', '-lole32', '-lgdi32', '-ldwmapi', '-luxtheme', '-loleacc', '-loleaut32', '-lwinhttp'))
    if ($Test) {
        $unitSources = @('tests/unit.cpp', 'src/model.cpp', 'src/store.cpp')
        Invoke-Native "$bin/clang++.exe" ($flags + @('-isystem', '.deps/json', '-Isrc', '-static', '-o', 'build/unit.exe') + $unitSources + @('-luser32'))
        Invoke-Native ./build/unit.exe @()
    }
} finally { Pop-Location }
