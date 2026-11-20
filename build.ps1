# Builds bin\MetroExodusRR.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs differently.
# Usage: .\build.ps1 [-Install] [-Package] [-GameDir <folder of MetroExodus.exe>] [-DlssdPath <nvngx_dlssd.dll>]
#   -Install  copies the add-on and nvngx_dlssd.dll next to MetroExodus.exe (game closed).
#   -Package  makes bin\release\MetroExodusRR-<version>.zip.
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Metro Exodus Enhanced Edition",
    # The Ray Reconstruction runtime (NVIDIA DLSS 310.9.16, not in the repository).
    [string]$DlssdPath = "E:\CallistoDLSS\bin\release\CallistoDLSS-1.0.3\nvngx_dlssd.dll"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "MetroExodus.exe"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat runs vswhere by name
# A stray quote in a PATH entry (seen: `GitHub CLI"`) breaks vcvars64.bat; drop quotes for this build.
$env:PATH = $env:PATH.Replace('"', '')
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\MetroExodusRR.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0. Only NGX headers are used
# (no NVIDIA library is linked): the game's own NGX session runs Ray Reconstruction.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /T cs_5_1 /E main /O3 /Vn kGiCenterCs /Fh `"$obj\gi_center_cs.h`" `"$root\src\gi_center.hlsl`" >nul",
    "fxc /nologo /T cs_5_1 /E main /O3 /Vn kCountResetCs /Fh `"$obj\count_reset_cs.h`" `"$root\src\count_reset.hlsl`" >nul",
    "fxc /nologo /T cs_5_1 /E main /O3 /Vn kGuideCs /Fh `"$obj\guide_cs.h`" `"$root\src\guide.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I `"$root\third_party\dlss\include`" /I `"$obj`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" `"$root\src\ngx.cpp`" `"$root\src\rt.cpp`" `"$root\src\rr.cpp`" `"$root\third_party\reshade\utils\descriptor_tracking.cpp`" user32.lib " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\MetroExodusRR.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

function Get-Dlssd {
    if (-not (Test-Path $DlssdPath)) { throw "nvngx_dlssd.dll not found at $DlssdPath (pass -DlssdPath)" }
    $sig = Get-AuthenticodeSignature $DlssdPath
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'NVIDIA') { throw "nvngx_dlssd.dll isn't NVIDIA-signed" }
    return $DlssdPath
}

if ($Install) {
    if (-not (Test-Path "$GameDir\$exe")) { throw "$exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process MetroExodus -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameDir -Force
    # The Ray Reconstruction model (the game ships only nvngx_dlss.dll); NGX loads it from the exe's folder.
    if (-not (Test-Path "$GameDir\nvngx_dlssd.dll")) {
        Copy-Item (Get-Dlssd) $GameDir
        Write-Host "Copied nvngx_dlssd.dll"
    }
    Write-Host "Installed to $GameDir"
}

if ($Package) {
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'MD_VERSION "([^"]+)"').Groups[1].Value
    $name = "MetroExodusRR-$version"
    $stage = "$root\bin\release\$name"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
    Copy-Item $addon, (Get-Dlssd) $stage
    Copy-Item "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $stage
    Copy-Item "$root\third_party\dlss\LICENSE.txt" "$stage\licenses\NVIDIA-DLSS-LICENSE.txt"
    $zip = "$root\bin\release\$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    # Forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "Packaged $zip"
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        "{0,-40} {1,-14} {2,12:N0}" -f $_.FullName.Substring($stage.Length + 1), $_.VersionInfo.FileVersion, $_.Length
    }
}
