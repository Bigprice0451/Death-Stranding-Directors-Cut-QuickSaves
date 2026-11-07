# Builds a test-only add-on and a hidden miniature D3D12 game on D:. It never
# opens Director's Cut and only sends F5 to its own disposable test window.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ReShadeDll,
    [string]$ScratchParent = 'D:\Codex Temporary'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($ScratchParent)
if ([IO.Path]::GetPathRoot($root) -ne 'D:\') { throw 'Use a scratch location on D:.' }
if (-not (Test-Path -LiteralPath $ReShadeDll -PathType Leaf)) { throw 'Provide the installed ReShade 6.8 dxgi.dll.' }
$project = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio x64 C++ tools are required.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') -ErrorAction Stop
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$work = Join-Path $root ('DSDC-thumbnail-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
Push-Location -LiteralPath $work
try {
    Copy-Item -LiteralPath $ReShadeDll -Destination (Join-Path $work 'dxgi.dll')
    $flags = @('/nologo','/O2','/std:c++17','/utf-8','/EHsc','/MD','/W4','/WX')
    & cl @flags "$PSScriptRoot\thumbnail_test_host.cpp" /Fe:host12.exe /link d3d12.lib dxgi.lib user32.lib
    if ($LASTEXITCODE) { throw 'Could not build the D3D12 test host.' }
    & cl @flags /DNOMINMAX /DWIN32_LEAN_AND_MEAN /LD "/I$project\src" "/external:I$project\vendor\reshade\include" "/external:I$project\vendor\imgui" /external:W0 "$PSScriptRoot\thumbnail_test_addon.cpp" "$project\src\thumbnail_image.cpp" "$project\src\thumbnails.cpp" "$project\src\quick_keys.cpp" /Fe:ThumbnailSmoke.addon64 /link windowscodecs.lib ole32.lib user32.lib
    if ($LASTEXITCODE) { throw 'Could not build the preview smoke add-on.' }
    & .\host12.exe
    if ($LASTEXITCODE) { throw "Test host stopped with code $LASTEXITCODE. See $work\ReShade.log." }
    $log = Get-Content -LiteralPath (Join-Path $work 'ReShade.log') -Raw
    foreach ($expected in @('MULTIBIND_F5_C_B_AND_SIMULTANEOUS','THUMB_F5_DETECTED','THUMB_RUNTIME_CLEAN','THUMB_GPU_TEST frame=6 magenta_pixels=6272 loaders=1 dimensions=160x120','THUMB_GPU_TEST frame=12 magenta_pixels=6272 loaders=2 dimensions=160x120')) {
        if ([regex]::Matches($log,[regex]::Escape($expected)).Count -ne 2) { throw "Two runtime lifetimes must both report '$expected'. See $work\ReShade.log." }
    }
    Write-Output "Two Direct3D 12 runtimes captured F5, rendered the expected thumbnail pixels, reused and replaced their cached texture, and released it. Evidence: $work\ReShade.log"
} finally {
    Pop-Location
}
