# Builds bin\MetroExodusRR.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs differently.
# Usage: .\build.ps1 [-Install] [-GameDir <folder of MetroExodus.exe>]
#   -Install  copies the add-on next to MetroExodus.exe (game closed).
param(
    [switch]$Install,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Metro Exodus Enhanced Edition"
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
# (no NVIDIA library is linked): the game's own DLSS calls are wrapped.
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

if ($Install) {
    if (-not (Test-Path "$GameDir\$exe")) { throw "$exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process MetroExodus -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameDir -Force
    # The Ray Reconstruction model (the game ships only nvngx_dlss.dll). NGX loads it from the exe's
    # folder. Taken from the CallistoDLSS release (NVIDIA DLSS SDK redistributable).
    $dlssd = "E:\CallistoDLSS\bin\release\CallistoDLSS-1.0.3\nvngx_dlssd.dll"
    if (-not (Test-Path "$GameDir\nvngx_dlssd.dll")) {
        if (-not (Test-Path $dlssd)) { throw "nvngx_dlssd.dll not found at $dlssd" }
        Copy-Item $dlssd $GameDir
        Write-Host "Copied nvngx_dlssd.dll"
    }
    Write-Host "Installed to $GameDir"
}
