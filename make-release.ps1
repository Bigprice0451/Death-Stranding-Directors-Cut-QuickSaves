[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ReShadeDll,
    [string]$BuildFolder=(Join-Path $PSScriptRoot 'build\Release'),
    [string]$OutputFolder=(Join-Path $PSScriptRoot 'release')
)
$ErrorActionPreference='Stop'
$runtime=Get-Item -LiteralPath $ReShadeDll
if($runtime.VersionInfo.FileVersion -notlike '6.8.0.*' -or $runtime.VersionInfo.ProductVersion -notlike '*UNOFFICIAL*'){
    throw 'Provide the source-built full x64 release of ReShade 6.8.0. See release_assets\PACKAGING-SOURCES.md.'
}
$addon=Get-Item -LiteralPath (Join-Path $BuildFolder 'DirectorCutQuickSaves.addon64')
$version=$addon.VersionInfo.FileVersion
if($version -notmatch '^\d+\.\d+\.\d+$'){throw 'The add-on needs a release version.'}
$assets=Join-Path $PSScriptRoot 'release_assets'
# Explicit archive names prevent private settings, Steam IDs, saves, logs,
# installed shaders, downloaded NVIDIA binaries, and build files from leaking.
$files=[ordered]@{
    'dxgi.dll'=$runtime.FullName
    'DirectorCutQuickSaves.addon64'=$addon.FullName
    'DirectorCutQuickSaves/DLSSRuntime.exe'=(Join-Path $BuildFolder 'DLSSRuntime.exe')
    'DirectorCutQuickSaves/READ ME.txt'=(Join-Path $assets 'READ ME.txt')
    'DirectorCutQuickSaves/THIRD-PARTY-NOTICES.txt'=(Join-Path $assets 'THIRD-PARTY-NOTICES.txt')
    'DirectorCutQuickSaves/NVIDIA-DLSS-LICENSE.txt'=(Join-Path $assets 'NVIDIA-DLSS-LICENSE.txt')
}
foreach($file in $files.Values){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing $file"}}
$folder=[IO.Directory]::CreateDirectory($OutputFolder).FullName
$output=Join-Path $folder ("Death Stranding Directors Cut - QuickSaves "+$version+'.zip')
$part=Join-Path $folder ([guid]::NewGuid().ToString('N')+'.zip.tmp')
Add-Type -AssemblyName System.IO.Compression.FileSystem
try {
    $zip=[IO.Compression.ZipFile]::Open($part,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach($name in $files.Keys){
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$files[$name],$name,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {$zip.Dispose()}
    $zip=[IO.Compression.ZipFile]::OpenRead($part)
    try {
        if($zip.Entries.Count -ne $files.Count){throw 'Unexpected archive contents'}
        foreach($name in $files.Keys){
            $entry=$zip.GetEntry($name)
            if(!$entry){throw "Missing entry $name"}
            $stream=$entry.Open()
            try {$hash=(Get-FileHash -InputStream $stream -Algorithm SHA256).Hash}
            finally {$stream.Dispose()}
            if($hash -ne (Get-FileHash -LiteralPath $files[$name] -Algorithm SHA256).Hash){throw "ZIP mismatch at $name"}
        }
    } finally {$zip.Dispose()}
    if([IO.File]::Exists($output)) {[IO.File]::Replace($part,$output,$null)}
    else {[IO.File]::Move($part,$output)}
    Get-Item -LiteralPath $output
} finally {
    if([IO.File]::Exists($part)){[IO.File]::Delete($part)}
}
