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

## 0.0.1 results (user run 2026-11-20, 4K, DLSS Balanced 2224x1248)

Logs in analysis/run1, shader disassembly in analysis/dis (dxc -dumpbin, 145 compute shaders, no
debug names in the DXIL).

DLSS:
- CreateFeature 1 (SR) on the game's NGX: render 2224x1248 -> 3840x2160, flags 0xb = IsHDR +
  MVLowRes + DepthInverted, PerfQuality 1. One evaluate per present on the main render thread.
- Evaluate: colour #a33a2 2224x1248 **r11g11b10f**, depth #3b624 r32g8x24 (D32S8, inverted),
  MV #a386a rg16f (= G-buffer RT3), exposure 1x1 r32f, mvscale = render size, sharpness 0.35,
  output rgba16f 3840x2160 mips12 (two outputs alternating). Jitter in pixels.
- Swapchain rgb10a2 HDR10 (colour space 3; RenoDX).
- **No DispatchRays**: ray tracing is inline (RayQuery) in compute shaders.

Frame (graphics + async compute queue, per frame ~17 lists):
- L4 G-buffer: RT0 #a3270 rgba8srgb, RT1 #a2d0f rgb10a2unorm (normals: composite reads it *2-1),
  RT2 #a3606 rgba8srgb, RT3 #a386a rg16f (motion vectors), depth #3b624.
- L7 (compute): csa48f6785 (prep, writes rg16snorm #97dcd + r32uint #98163), then
  **csb425d82a = GI ray trace, 139x78 groups of 16x16 = full render resolution, 1 ray/pixel**,
  output r32uint #97f98 (packed hit data: 16-bit distance + direction/material bits, not radiance).
- L11 lighting into two r11g11b10f targets #a33a2 + #a3738 (diffuse/specular split, likely).
- **L13 (compute) = the RT denoiser chain**, all full render resolution:
  1. cs1fce2042 (tacbm3?): shades the GI hits (cubemaps, probe 2darray) **and** accumulates over
     time -> #97eff rgba16f. Shading and temporal accumulation are one pass: it can't simply be
     skipped.
  2. csb83925ed (blur): two mip chains #6a316 + #6b0e0 rgba16f mips12, + r32f #97e66.
  3. cs648ac480 (mips34, 18x10 groups).
  4. cs1e664fee (gapfix/noblur).
  5. cs66e47af2: final -> #98460 + #9832e rgba16f.
- L14: csa11d90f8 tile classify (70x39 tiles of 32x32), **cs57d4ae57 = reflection trace**
  (RayQuery, 2691 tiles of 32x32 = full res).
- L15: **cs56190203 = composite**: reads the denoised GI and the traced reflections (t8, blended
  by |alpha| > 0.01) and **adds** them into the two lighting targets (u0/u1 read-modify-write).
  Then cs576e2cf3 (another RayQuery pass, 2691 tiles), transparents/fog into #a33a2, DLSS (L16),
  post (bloom chain 1112x624..34x19) and tonemap to the back buffer (L17, present thread).

### Feasibility for Ray Reconstruction

Good: GI and reflections are traced at full render resolution with one ray per pixel, and the
denoiser is a separate chain whose result is only added in one composite pass. G-buffer guides
exist (albedo/specular/normals/MV/depth).
Hard part: the GI hit shading lives inside the temporal pass, so "denoiser off" means keeping
that pass but with history weight 0 (constants in its root CBV, or a patched shader) and skipping
the spatial passes 2-4.

Next (0.0.2): live experiments in the overlay — skip passes 2-4, dump/patch cs1fce2042's
constants to find the history weight — and decode the G-buffer channels (albedo, roughness,
metalness) for RR's guides. Then 0.1.0: create the RR feature (dlssd) on the game's NGX and
evaluate it instead of SR.
