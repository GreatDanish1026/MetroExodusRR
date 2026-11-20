# MetroExodusRR findings

Goal: NVIDIA DLSS Ray Reconstruction in Metro Exodus Enhanced Edition, as a ReShade add-on:
turn off the game's own ray-tracing denoisers and replace its DLSS Super Resolution call with a
Ray Reconstruction call that gets the noisy lit picture plus G-buffer guides.

Started 2026-11-20 as a copy of E:\MetroDLSS (Metro 2033 Redux probe, same 4A Engine and the same
statically linked NGX). MetroDLSS's docs/FINDINGS.md has the hook design and the 2033 results.

## Game (static look, 2026-11-20)

- Steam, `E:\SteamLibrary\steamapps\common\Metro Exodus Enhanced Edition\MetroExodus.exe`, PE
  timestamp 0x6a9046b0, `.bind` section = SteamStub (code encrypted on disk, import table normal).
- **Native D3D12 renderer** (ReShade.log: D3D12CreateDevice, RenoDX hooks a d3d12 device), unlike
  Metro 2033 Redux which draws with D3D11. Ray tracing is always on (RT GI and reflections).
- **NGX linked statically** (path in exe: `NGX\core\rel_1_6`): NVSDK_NGX_D3D12_* names as strings,
  KERNEL32 GetProcAddress imported, so MetroDLSS's import-table hook applies unchanged.
- `nvngx_dlss.dll` already swapped to a 310.x build (74 MB, original kept as
  `nvngx_dlss.dll.dlsss` by DLSS Swapper). No `nvngx_dlssd.dll` (Ray Reconstruction) in the folder.
- Cvars in the exe: `r_dlss`, `r_dlss_rx`, `r_dlss_sharpness_rx`, `r_tsr_upscale`.
- Denoiser passes named in the exe (shader cache keys): `dxrt_denoise2_blur_cs/undersample=%d`,
  `dxrt_denoise2_gapfix_cs`, `dxrt_denoise2_mips34_cs`, `dxrt_denoise2_noblur_cs`,
  `dxrt_denoise2_tacbm3_cs/undersample=%d`. "undersample" suggests GI is traced below render
  resolution; Ray Reconstruction wants per-pixel noise at render resolution, which is the main
  open risk.
- Already installed by the user: ReShade 6.8.0 as `dxgi.dll`, `renodx-metroexodus.addon64`,
  `PSOBoost.addon64` (separate shader-compile probe), Ultimate ASI Loader as `version.dll`
  (no-intro mod).

## 0.0.1 probe

Same as MetroDLSS 0.0.1 (NGX Create/Evaluate/Release log, F10 two-frame capture with NGX EVALUATE
marker) plus:
- every compute shader saved once to `MetroExodusRR-shaders\csXXXXXXXX.dxil`, with its ILDN debug
  name (if any) in the log;
- `DISPATCH RAYS w x h` lines in the capture.

To find: the DLSS evaluate inputs, where the RT passes and the `dxrt_denoise2_*` dispatches sit,
the G-buffer layout (albedo, normals, roughness/metalness), and whether the noisy GI/reflection
signal can be fed straight into lighting at render resolution.
