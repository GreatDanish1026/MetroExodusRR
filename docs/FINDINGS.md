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

Correction after reading the shaders more closely: cs1fce2042 has **no history input**; it only
shades the GI hits (8-sample loop, result x pi, hit distance in alpha), so #97eff is the raw,
noisy per-pixel GI radiance. The temporal accumulation is csb83925ed (7 inputs, writes mips 0-2
of the two rgba16f chains), and cs66e47af2 is an 8-tap bilateral blur over the two chains.

## 0.0.2 (experiments, all off by default)

rt.cpp follows each command list's compute state and resolves the known passes' t#/u# textures
through ReShade's descriptor_tracking (binding map logged once per pass, again on F11). F11 also
writes each pass's root CBV to `MetroExodusRR-cb-<pass>-p<param>.bin`. Overlay: clear GI history
(temporal inputs that the final blur wrote last frame), replace the final blur by a copy of t3/t4
(optionally swapped), skip the temporal/mips/gapfix/blur passes, and a texture viewer for any
bound texture of a known pass (copied on the present queue, state from the last observed
barrier; per-mip barriers aren't distinguished, so mip-chain states are a guess).

### 0.0.2 run (2026-11-20, analysis/run2): binding maps

No crash; all ten known passes seen with the same shader ids. GI data flow (resource ids differ
per run):
- gi-prep: G-buffer (t1 rgba8srgb RT0, t2 rgb10a2 normals RT1, t3 rgba8srgb RT2, t4 depth, blue
  noise 64x64x64) -> u1 rg16snorm = **ray direction** (octahedral), u2 r32uint, u0 r32uint.
- gi-trace: normals, depth, ray dir -> u0 r32uint hit data.
- gi-hit-shading: hit data + ray dir + probe grid (8x9216 r32uint, 1584x144, 3168x288),
  cubemaps, sun shadow maps (3x 4096^2) -> u0 **#raw rgba16f = noisy GI radiance + hit distance**.
- gi-temporal: t0 MV, t1 ray dir, t2 depth, t3 r32f (history length/moments, ping-pong with u0),
  t4 raw GI, **t5/t6 history = last frame's blur outputs** -> mips 0-2 of two rgba16f chains A/B.
  Two rgba16f with the ray direction = GI stored as a directional encoding (SH-like, likely
  YCoCg: A = luma, B = chroma); the composite evaluates it with the pixel normal.
- gi-mips (mips 3-4), gi-gapfix (r32f t0 + chains, in place), gi-spatial-blur (normals, r32f,
  chains A/B -> u0/u1 = the denoised GI = composite t9/t10 and next frame's history).
- reflection-trace -> u0 **rgba32f reflection radiance**, read directly by the composite (t5):
  reflections get **no spatial/temporal denoiser** of their own (DLSS SR cleans them up today).
  rt-after-composite writes the same rgba32f again after the composite (next frame / transparents).
- rt-composite: G-buffer RT0/RT1/RT2, depth, reflections, denoised GI A/B, cubemaps -> adds into
  the two r11g11b10f lighting targets; DLSS colour is one of them (u0 here).

Plan for "denoiser off" that keeps the game's own GI encoding: clear the temporal pass's history
(t5/t6) and its r32f history length (t3) so it keeps only the current sample, skip gi-mips and
gi-gapfix, and replace gi-spatial-blur by a copy of mip 0 of A/B. The composite then adds noisy
1-sample GI + raw reflections, which is what Ray Reconstruction wants as colour input.

### 0.0.2 experiments (user, by eye)

- Clear history: whole picture **darker**. Unexplained so far: the temporal pass keeps its
  per-pixel frame count in the low mantissa bits of the history (chain B .x/.y low 4 bits, and the
  r32f output = depth bits with count in the low 5 bits, count capped at 31); with count 0 the
  blend weight works out to 1 (current sample only), which should be noisy, not darker.
- Blur replaced by a copy: picture **noisy** (the spatial blur does most of the denoising).
- Skipping temporal/mips/gapfix alone: no visible change (stale data stays in place).

0.0.3: one "GI denoiser OFF" switch (all four at once) and a "measure brightness" button (average
RGB of every rgba16f/rgba32f/r11g11b10f texture of the viewer's pass, logged) to check whether
1-sample GI keeps the game's brightness.

### 0.0.3 measurements (analysis/run3, rt-composite textures read at present, dark scene)

| texture | denoiser on | denoiser OFF |
|---|---|---|
| u0 lit colour (DLSS input) lum | 0.0119 (max 0.90) | 0.0069 (max 0.54), **-42 %** |
| t9 GI A lum | 0.0003 | 0.0000 (max 0.21: sparse spikes) |
| t10 GI B (b channel) | 0.0827 | 0.0018 |
| t5 reflections lum | 0.0002 | 0.0000 |

Cause, from cs66e47af2's end: the chains are stored **without** a per-pixel factor
f = saturate(B.z * 12) * 0.9 + 0.1 (the temporal pass divides history by f, the blur multiplies
its weighted average by f again). Output A = f * avg(A); output B = (f * B.x', f * B.y', B.z,
count/15) where B.x'/B.y' have their low 4 bits forced to 7 (they carry packed count bits) and
count = low 4 bits of B.y. A plain copy of mip 0 skips the f multiply (f >= 0.1, so up to 10x too
dark) and keeps the packed bits: that is the darkening, not lost energy.

Fix: replace the blur with a centre-only shader of our own that does exactly that output step
(0.0.4), then measure again.

### 0.0.4 measurements (analysis/run4): centre-only pass, still darker

Denoiser on: lit u0 lum 0.0359, GI B (t10) b 0.0830. Denoiser OFF (history cleared + mips/gapfix
skipped + centre-only output): lit 0.0233 (**-35 %**, user also sees it dark), GI B b 0.0011.
The centre pass ran (log). B.z (which sets f) collapses, so the loss now comes from upstream:
most likely the temporal pass with cleared history (B.z = lerp(history B.z, raw .w, w)), not the
output step. 0.0.5 adds a sweep that measures each combination (game, centre only, centre +
skips, clear history only, all) after 90 frames each, to pin it down.

### 0.0.5 sweep (analysis/run5): the culprit is clearing the history

| combination | lit u0 lum | GI B (t10) b |
|---|---|---|
| game | 0.0360 | 0.0830 |
| centre-only blur | 0.0364 | 0.0795 |
| centre-only + skip mips/gapfix | 0.0361 | 0.0805 |
| clear history only | **0.0249** | 0.0004 |
| all | **0.0250** | 0.0011 |

The centre-only pass and the skips keep the brightness; clearing the history kills the GI.
Reason (csb83925ed start): the frame count is **not** in the colour history. The pass gathers its
t3 = last frame's r32f (depth with the count in the low 5 mantissa bits), checks depth for
disocclusion (|d - d0| / d < 0.04) and interpolates the counts. Cleared colour + kept count = the
pass trusts a black history and the GI fades towards 0.

0.0.6: "reset frame count" zeroes the low 5 bits of t3 (count_reset.hlsl, in place, before the
temporal pass): weight 1 = current frame only, depth test intact. "GI denoiser OFF" = reset count
+ skip mips/gapfix + centre-only output. The sweep's 4th row is now "reset frame count only".

### 0.0.6 sweep (analysis/run6)

| combination | lit u0 lum |
|---|---|
| game | 0.0361 |
| centre-only blur | 0.0361 |
| centre-only + skip mips/gapfix | 0.0359 |
| reset frame count only | 0.0356 (-1 %) |
| all (denoiser OFF) | 0.0303 (**-16 %**) |

Both passes run (log). Each change alone is within 1 %; only the full combination loses 16 %:
f = saturate(B.z * 12) * 0.9 + 0.1 of a single noisy B.z sample is biased low (clipping; the
game's blur computes f from blurred B.z). 0.0.7: the centre pass takes B.z (and f) from a 5x5
stride-2 average of B.z; A, B.x, B.y stay the centre pixel (noisy GI colour for RR).

### 0.0.7 sweep (analysis/run7)

game 0.0360; centre-only 0.0371 (+3 %); centre + skips 0.0367; reset count only 0.0353 (-2 %);
all 0.0315 (**-12.5 %**). Averaging B.z only recovered a quarter of the loss, so most of the bias
comes from the temporal pass itself when it gets single samples with no spatial filter after it
(its YCoCg/SH conversion clamps, e.g. max(luma, 0), and compresses with 1/(1 + luma)).

0.0.8: the reset became a **cap** (count = min(count, N), slider 0-31, push constant), and the
sweep now runs: game, spatial off, spatial off with cap 7/3/1/0, cap 0 with the spatial filter on.
A small cap may give RR an almost-raw signal without the brightness loss.

### 0.0.8 cap sweep (analysis/run8)

| combination | lit u0 lum | vs game |
|---|---|---|
| game | 0.0360 | |
| spatial off (centre + skips) | 0.0367 | +2 % |
| spatial off, cap 7 | 0.0359 | -0.3 % |
| **spatial off, cap 3** | 0.0354 | **-1.7 %** |
| spatial off, cap 1 | 0.0344 | -4.4 % |
| spatial off, cap 0 | 0.0317 | -12 % |
| cap 0, spatial on | 0.0347 | -3.6 % |

Decision: RR input = spatial filter off + frame count capped at **3** (default of the slider now):
about four frames of history, unfiltered per pixel, within 2 % of the game's brightness.
Reflections are already raw.

## 0.1.0 plan: Ray Reconstruction

1. RR DLL: `nvngx_dlssd.dll` next to MetroExodus.exe (copy from E:\CallistoDLSS release; NGX
   loads feature DLLs from the app folder).
2. In the Evaluate wrapper: create an RR feature (NVSDK_NGX_Feature_RayReconstruction) through
   the driver's own CreateFeature/AllocateParameters that the game looked up (game's NGX session,
   game's command list), same render/output size, flags HDR | MVLowRes | DepthInverted; evaluate
   it instead of SR with the game's colour/depth/MV/jitter/exposure plus guides; SR kept as
   fallback and A/B switch.
3. Guides from the G-buffer (decoding still to read from gi-prep/composite): diffuse albedo
   (RT0 rgba8srgb), normals (RT1 rgb10a2, *2-1; check view vs world space), roughness + specular
   (RT2 rgba8srgb), specular albedo = F0 * env-BRDF approximation. A small compute pass writes
   them into our own textures each frame before the evaluate.
4. RR needs world-to-view and view-to-clip matrices: from the composite's root CBV (registers
   17-20 reconstruct position from depth) — to be decoded from the F11 dumps.
5. Denoiser off (cap 3 + skips + centre) is switched on together with RR.

### Camera and G-buffer (from the composite's code and CB dump, run2)

Composite root CBV (param 1), column vectors: registers 7-9 = world->view 3x4 (translation in w),
10-12 = view->world (rotation; camera position in w), 13-16 = projection (row 13 = 0.913, 0,
jitter, 0; row 14 = 0, 1.732, jitter, 0; row 15 = 0, 0, -0.00009, 0.1; row 16 = 0, 0, 1, 0: view z
forward, inverted depth with near 0.1, far = 0), 17-20 = inverse projection (clip -> view).
G-buffer: RT0 rgb = diffuse albedo, a = (occlusion-like, >0.0118 used); RT1 rgb = **view-space**
normal *0.5+0.5, a = material id/3 (id 2 = F0 0.05, id 3 = roughness tweak); RT2 r = gloss
(roughness = 1 - r), gba = specular F0. Depth = D32S8 typeless, SRV as r32_float_x8.

## 0.1.0: Ray Reconstruction (first build, untested)

- rt.cpp RunGuides at the composite dispatch (graphics list, just before DLSS): guide.hlsl makes
  diffuse albedo, env-BRDF specular albedo and world normal + roughness (rgba16f, render size);
  the matrices for NGX are built from registers 7-9 (WorldToView, row-vector form) and 13-16
  (ViewToClip = transpose, jitter removed).
- rr.cpp: in the game's Evaluate, creates NVSDK_NGX_Feature_RayReconstruction in the game's NGX
  session (driver CreateFeature, own AllocateParameters block; game's flags 0xb and quality, HW
  depth, packed roughness) and evaluates it with the game's colour/depth/MV/jitter/exposure/output
  instead of the SR call; any failure -> the game's SR runs. NVSDK_NGX_Parameter_Set* and
  CreateFeature/EvaluateFeature_C are defined in rr.cpp on top of the driver so NVIDIA's helper
  macros work without the NGX library.
- While RR runs, the GI denoiser is off (cap 3 + skips + centre pass).
- nvngx_dlssd.dll 310.9.16 copied next to the exe by build.ps1 -Install (from CallistoDLSS 1.0.3).
- Open risk: the game's NGX is SDK rel_1_6 (old); the driver may refuse RR in that session.

Run 1 (analysis/run9): **RR works in the game's NGX session.** "Ray Reconstruction created:
2224x1248 -> 3840x2160, quality 1, flags 0xb", evaluated every frame for ~75 s with no failure;
guides, frame count reset and centre pass all running. One "G-buffer state 0" on the very first
composite (barrier not seen yet), fine from the next frame. Projection that run: P00 1.1547,
P11 1.7321 (P00 differs from run2's 0.913: read live every frame, so fine).
User verdict: the overlay showed "running: 2224x1248 -> 3840x2160" and **the picture looked good**
(no smearing, colour or flicker problems reported).

Next (0.0.2): live experiments in the overlay — skip passes 2-4, dump/patch cs1fce2042's
constants to find the history weight — and decode the G-buffer channels (albedo, roughness,
metalness) for RR's guides. Then 0.1.0: create the RR feature (dlssd) on the game's NGX and
evaluate it instead of SR.
