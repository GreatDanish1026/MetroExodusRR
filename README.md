# MetroExodusRR 1.0.0

ReShade add-on for **Metro Exodus Enhanced Edition** (4A Engine, DirectX 12) that replaces the
game's DLSS Super Resolution with **NVIDIA DLSS Ray Reconstruction**: one AI model denoises the
game's ray-traced global illumination and reflections and upscales, in place of the game's own GI
denoiser and DLSS. The game ships DLSS 2 but no Ray Reconstruction.

Needs an NVIDIA RTX graphics card, ReShade 6.8 or later with add-on support, and **DLSS** selected
in the game's options (any DLSS quality mode).

## What it does

- **Ray Reconstruction** (on by default): the game's DLSS call is replaced by Ray Reconstruction
  with the same inputs (picture, depth, motion vectors, jitter, exposure) plus what Ray
  Reconstruction needs from the game's G-buffer: diffuse and specular albedo, normals and
  roughness, made by the add-on every frame, and the camera. It runs inside the game's own DLSS
  session. If it can't run for any reason, the game's DLSS is used as usual.
- **The game's GI denoiser goes off while Ray Reconstruction runs**: its spatial filter passes are
  skipped and its temporal accumulation is capped at a few frames, so Ray Reconstruction gets the
  ray-traced GI close to raw. The reflections have no denoiser of their own in this game: they go to
  Ray Reconstruction as traced. With Ray Reconstruction off, the game's denoiser is untouched.
- **Ray Reconstruction model**: NVIDIA's default, D, E or F (transformer models; F is the latest).
- **GI history frames** (default 3): how many frames the game's GI accumulation keeps before Ray
  Reconstruction. Fewer is rawer, but the game's GI encoding then loses brightness (0 is about
  12 % darker); 3 keeps the picture within 2 % of the game's own brightness.

The overlay (ReShade's Add-ons tab) shows the settings and the status; **Advanced** has testing
tools (frame capture, pass list, texture viewer, brightness measurements). Settings are saved in
`ReShade.ini` under `[MetroExodusRR]`.

## Install

1. Install ReShade 6.8 or later with full add-on support for `MetroExodus.exe` (DirectX 12), in
   the game's folder (`...\steamapps\common\Metro Exodus Enhanced Edition`).
2. Copy `MetroExodusRR.addon64` and `nvngx_dlssd.dll` from the release zip into that folder.
3. In the game: DLSS on (any quality mode). Ray Reconstruction starts on its own; open ReShade
   (Home key), Add-ons tab, MetroExodusRR to switch it off or change the model. The status line
   reads "Ray Reconstruction running" when it works.
4. A Ray Reconstruction override for the game in the NVIDIA App takes precedence over the model
   setting.

Works alongside RenoDX's Metro Exodus add-on (HDR). The zip includes NVIDIA's DLSS Ray
Reconstruction 310.9.16 runtime (`nvngx_dlssd.dll`), signed by NVIDIA; its licence is in
`licenses`, attribution in `THIRD_PARTY_NOTICES.txt`. To uninstall, delete the two files. Log:
`MetroExodusRR.log` next to the exe.

Tested with the Steam version at 3840x2160 (DLSS Balanced, 2224x1248) on an NVIDIA RTX 5080 with
RenoDX HDR.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools, Windows SDK for `fxc`). `.\build.ps1 -Install` copies
the add-on and `nvngx_dlssd.dll` into the game folder (game closed); `.\build.ps1 -Package` makes
the release zip in `bin\release`. `nvngx_dlssd.dll` isn't in the repository: pass `-DlssdPath`
(NVIDIA's DLSS 310.9.16 release, `lib\Windows_x86_64\rel\nvngx_dlssd.dll`); the script checks
NVIDIA's signature.

## How it works

`docs/FINDINGS.md` has the details: how the game's DLSS calls are reached (its NGX is linked
statically and looks up the driver's functions through `GetProcAddress`, which the add-on hooks),
the frame's ray-tracing passes (inline RayQuery in compute shaders), the GI denoiser chain and why
it is capped rather than cleared (the frame count lives in the low bits of a depth texture, the GI
is stored divided by a brightness factor), the G-buffer layout and the camera in the composite
pass's constants, and the measurements behind each choice. `Debug=1` under `[MetroExodusRR]` in
`ReShade.ini` turns on the F10 frame capture / F11 dump hotkeys and saves the game's compute shaders
for disassembly.
