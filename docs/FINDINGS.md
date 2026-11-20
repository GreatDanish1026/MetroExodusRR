# MetroDLSS findings

Goal: NVIDIA DLSS Frame Generation (Streamline DLSS-G) in Metro 2033 Redux, next to the game's
own DLSS Super Resolution, as a ReShade add-on.

## Game (2026-10-31)

- Steam, `E:\SteamLibrary\steamapps\common\Metro 2033 Redux\metro.exe`, PE timestamp
  0x6adb4a67 (2026-10-23), image size 0x15a9000, 4A Engine. `.bind` section = SteamStub DRM
  (code encrypted on disk; the import table is normal).
- Renders with **D3D12** (ReShade.log: D3D11 test devices, then D3D12CreateDevice + queue).
- Ships `nvngx_dlss.dll` 310.9.16, `nvngx_dlssnr.dll` 310.9.16 and `amd_fidelityfx_dx12.dll`
  1.0.1.41314 (FSR upscaling only: no frame-interpolation strings in the exe).
- **NGX is linked statically** (nvsdk_ngx_s): the exe holds the NGX function names as strings
  and resolves them in the driver's `_nvngx.dll` with GetProcAddress (imported from KERNEL32 by
  metro.exe). No Streamline, no DLSS-G, **no Reflex** in the exe.
- ReShade 6.8.0 installed as `dinput8.dll` (DisabledAddons: Generic Depth, Effect Runtime Sync).

## Approach

Wrap the game's NGX calls (Create/Evaluate/Release, same signature on the SDK and driver side)
by replacing metro.exe's GetProcAddress import: the game gets permanent jump stubs whose target
is the add-on's wrapper while it is loaded and the driver function otherwise. Evaluate gives
every DLSS input frame generation needs (depth, motion vectors, jitter, MV scale, render size)
at record time on the game's command list.

Still to find: camera matrices (Streamline constants), a pre-HUD copy of the frame (hudless),
and where to put Reflex markers (the game has none).

Lessons from E:\HellbladeDLSS (also native static NGX + Streamline FG): the game's NGX receives
ReShade proxy objects; after Streamline initialised NGX on the native device, the game's NGX
calls needed the native command list (unwrapped with ReShade's IID
{7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}).

## 0.0.1 probe

Logs NGX lookups, CreateFeature parameters (sizes, flags, presets), Evaluate inputs (textures,
subrects, jitter, MV scale, reset, exposure), new ring-buffer textures, first 32 jitters and a
summary every 1200 presents. F10 writes MetroDLSS-frame-N.txt: two frames of GPU work with
shader ids (cs/vs/ps), small draws listed one by one, root constants, and the NGX EVALUATE
position.

## 0.0.1 results (user run 2026-10-31, 4K, DLSS at 2227x1253 = 58 %)

**Correction: the game renders with D3D11.** The D3D12 device exists only to run the
upscalers (DLSS via NGX, FSR via the FidelityFX DX12 SDK) on textures shared with D3D11.
- Swapchain: IDXGIFactory::CreateSwapChain on the **D3D11** device, R8G8B8A8_UNORM,
  BufferCount 3, SwapEffect 0 (DISCARD, legacy blt model), SDR (colour space 1). Recreated on
  settings changes (3 times in one run, NGX features released and recreated with them).
- Frame: D3D11 immediate context draws the scene at render size (G-buffer rgba16f x2 + rgba8srgb
  + rg16f, depth r24g8), then one D3D12 command list (thread 16440, ~2 lists/frame) does:
  barriers, **DLSS evaluate** (colour #d0481 2227x1253 rgba16f, depth #dfd7b r32f, MV #e0078
  rgba16f, output #d0817 3840x2160 rgba16f), game compute csbf9eda9d (480x270 groups) on
  3840x2160 rgba16f, then **feature 18** (unnamed "Reserved18", evaluated with no inputs; uses
  7680x4320 r16f and 3840x2160 rgba16f textures), compute cs364b1a77, barriers back to general.
- Back on D3D11 (thread 32008, the present thread): back buffer 3840x2160 rgba8unorm bound,
  **one full-screen DRAW 3 (ps46a32745)** reads the upscaled 3840x2160 rgba16f (#6761a) + bloom
  etc. = tonemap/composite, then **HUD**: DRAW 6 (ps3d4a6083) x2, DRAW indexed 6 (ps04751e34),
  then Present. So the hudless picture is the back buffer right after the ps46a32745 draw.
- DLSS create: feature 1, flags 0x3 = **IsHDR + MVLowRes** (MVs not jittered, depth not
  inverted), PerfQuality 1 (Balanced), presets J/K/L-range values (0x8000000b..d).
- Evaluate: mvscale 2227,1253 (MVs in UV units), jitter in pixels, reset 1 only on first frame
  after create, pre-exposure 1, same texture every frame (no ring). Jitter sequence Halton-like, a
  new value every frame (the probe's list shows each twice only because the feature-18 call has
  no jitter and the probe kept the previous value).
- About 100 presents per second, one DLSS evaluate per present.
- ReShade gives NGX proxy command lists (unwrap works).

### Consequence for frame generation

DLSS-G (and FSR FG) only present through a D3D12 (or Vulkan) swapchain. Plan: a D3D12
presentation bridge. Create the real window swapchain on the game's D3D12 device/queue through
Streamline, give the game a D3D11 swapchain stand-in whose back buffers are textures shared
with D3D12, and at the game's Present: copy the hudless after ps46a32745, signal D3D11 -> D3D12
fence, copy into the D3D12 back buffer, tag depth/MV (copied at evaluate time) + hudless, and
Present through Streamline. Also needed: camera matrices (D3D11 constant buffers; not in the
DLSS parameters), Reflex markers (game has none), and ReShade's own overlay/effects moving to
the D3D12 swapchain. E:\HellbladeDLSS has a tested DXGI swapchain facade (HDR bridge) to borrow.
