// DLSS Ray Reconstruction in place of the game's DLSS Super Resolution.
//
// The game's statically linked NGX already runs a session on its D3D12 device (the driver's NGX
// module, looked up through the hooked GetProcAddress, ngx.cpp). This file creates a Ray
// Reconstruction feature in that same session and, in the game's Evaluate call, evaluates it with
// the game's own inputs (colour, depth, motion vectors, jitter, exposure, output) plus the G-buffer
// guides and camera made at the RT composite pass (rt.cpp). If RR is off, the guides are stale or
// anything fails, the game's own Super Resolution call runs as usual.
//
// No NVIDIA library is linked: NVIDIA's header helpers (NGX_D3D12_CREATE_DLSSD_EXT /
// NGX_D3D12_EVALUATE_DLSSD_EXT) need the NVSDK_NGX_Parameter_Set* functions and
// NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature_C, defined below on top of the parameter object's
// own interface and the driver's functions. The RR model itself is nvngx_dlssd.dll next to the exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <nvsdk_ngx_helpers_dlssd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "common.hpp"

// ---------------------------------------------------------------- NGX SDK functions the helpers use

extern "C" {
void NVSDK_CONV NVSDK_NGX_Parameter_SetULL(NVSDK_NGX_Parameter* p, const char* n, unsigned long long v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* p, const char* n, float v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD(NVSDK_NGX_Parameter* p, const char* n, double v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p, const char* n, unsigned int v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter* p, const char* n, int v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d11Resource(NVSDK_NGX_Parameter* p, const char* n, ID3D11Resource* v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter* p, const char* n, ID3D12Resource* v) { p->Set(n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* p, const char* n, void* v) { p->Set(n, v); }
}

using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using AllocateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p,
                                                          NVSDK_NGX_Handle** out) {
    auto f = reinterpret_cast<CreateFn>(ngx::DriverFunction(ngx::kDriverCreate));
    return f ? f(cl, id, p, out) : NVSDK_NGX_Result_FAIL_NotInitialized;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* h,
                                                              const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback_C) {
    auto f = reinterpret_cast<EvaluateFn>(ngx::DriverFunction(ngx::kDriverEvaluate));
    return f ? f(cl, h, p, nullptr) : NVSDK_NGX_Result_FAIL_NotInitialized;
}

namespace rr {
namespace {

std::mutex g_mutex;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_handle = nullptr;
unsigned g_renderW = 0, g_renderH = 0, g_outW = 0, g_outH = 0;
bool g_disabled = false;  // creation failed for these sizes: don't retry every frame
uint64_t g_lastSeq = 0;
uint64_t g_evaluates = 0;
int g_failures = 0;
std::string g_status = "waiting for the game's DLSS";
bool g_reset = true;

ID3D12Resource* Res(const NVSDK_NGX_Parameter* p, const char* name) {
    ID3D12Resource* r = nullptr;
    return p->Get(name, &r) == NVSDK_NGX_Result_Success ? r : nullptr;
}

float F(const NVSDK_NGX_Parameter* p, const char* name, float def) {
    float v = def;
    return p->Get(name, &v) == NVSDK_NGX_Result_Success ? v : def;
}

unsigned U(const NVSDK_NGX_Parameter* p, const char* name, unsigned def) {
    unsigned v = def;
    return p->Get(name, &v) == NVSDK_NGX_Result_Success ? v : def;
}

void ReleaseFeature() {  // g_mutex held
    if (!g_handle) return;
    if (auto f = reinterpret_cast<ReleaseFn>(ngx::DriverFunction(ngx::kDriverRelease))) f(g_handle);
    g_handle = nullptr;
}

bool Fail(const std::string& why) {  // g_mutex held
    if (why != g_status) Log("Ray Reconstruction: %s", why.c_str());
    g_status = why;
    rt::SetRrWorking(false);
    return false;
}

std::string ResultText(NVSDK_NGX_Result r) { return Format("0x%08x", static_cast<unsigned>(r)); }

}  // namespace

bool Evaluate(void* commandList, const void* gameParams) {
    if (!rt::RrWanted()) {
        rt::SetRrWorking(false);
        return false;
    }
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(commandList);
    const auto* gp = static_cast<const NVSDK_NGX_Parameter*>(gameParams);
    if (!cl || !gp) return false;
    std::lock_guard<std::mutex> lock(g_mutex);

    rt::Guides g = {};
    if (!rt::LatestGuides(&g)) return Fail("no guides yet (RT composite pass not seen)");
    if (g.seq == g_lastSeq) return Fail("guides not refreshed this frame: game's DLSS used");

    ID3D12Resource* color = Res(gp, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* output = Res(gp, NVSDK_NGX_Parameter_Output);
    ID3D12Resource* depth = Res(gp, NVSDK_NGX_Parameter_Depth);
    ID3D12Resource* motion = Res(gp, NVSDK_NGX_Parameter_MotionVectors);
    if (!color || !output || !depth || !motion) return Fail("game's DLSS inputs missing");
    const unsigned renderW = U(gp, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0);
    const unsigned renderH = U(gp, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0);
    const D3D12_RESOURCE_DESC od = output->GetDesc();
    const unsigned outW = static_cast<unsigned>(od.Width), outH = od.Height;
    if (!renderW || !renderH) return Fail("render size unknown");

    if (!g_params) {
        auto alloc = reinterpret_cast<AllocateFn>(ngx::DriverProc("NVSDK_NGX_D3D12_AllocateParameters"));
        if (!alloc) return Fail("NVSDK_NGX_D3D12_AllocateParameters not found");
        const NVSDK_NGX_Result r = alloc(&g_params);
        if (NVSDK_NGX_FAILED(r) || !g_params) {
            g_params = nullptr;
            return Fail("couldn't allocate NGX parameters (" + ResultText(r) + ")");
        }
    }

    const bool sizesChanged = renderW != g_renderW || renderH != g_renderH || outW != g_outW || outH != g_outH;
    if (sizesChanged) g_disabled = false;
    if (g_disabled) return false;
    if (!g_handle || sizesChanged) {
        ReleaseFeature();
        NVSDK_NGX_DLSSD_Create_Params cp = {};
        cp.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;  // roughness in the normals' w
        cp.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        cp.InWidth = renderW;
        cp.InHeight = renderH;
        cp.InTargetWidth = outW;
        cp.InTargetHeight = outH;
        cp.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(ngx::GameQuality());
        cp.InFeatureCreateFlags = static_cast<int>(ngx::GameCreateFlags()) & ~NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
        cp.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSD_EXT(cl, 1, 1, &g_handle, g_params, &cp);
        g_renderW = renderW, g_renderH = renderH, g_outW = outW, g_outH = outH;
        if (NVSDK_NGX_FAILED(r) || !g_handle) {
            g_handle = nullptr;
            g_disabled = true;
            return Fail("couldn't create the Ray Reconstruction feature (" + ResultText(r) +
                        "); is nvngx_dlssd.dll next to MetroExodus.exe?");
        }
        Log("Ray Reconstruction created: %ux%u -> %ux%u, quality %d, flags 0x%x", renderW, renderH, outW, outH,
            ngx::GameQuality(), cp.InFeatureCreateFlags);
        g_reset = true;
    }

    // NGX keeps the matrix pointers in the parameter block after the call: static storage.
    static float worldToView[16], viewToClip[16];
    std::memcpy(worldToView, g.worldToView, sizeof(worldToView));
    std::memcpy(viewToClip, g.viewToClip, sizeof(viewToClip));

    NVSDK_NGX_D3D12_DLSSD_Eval_Params ep = {};
    ep.pInColor = color;
    ep.pInOutput = output;
    ep.pInDepth = depth;
    ep.pInMotionVectors = motion;
    ep.pInExposureTexture = Res(gp, NVSDK_NGX_Parameter_ExposureTexture);
    ep.pInDiffuseAlbedo = static_cast<ID3D12Resource*>(g.diffuse);
    ep.pInSpecularAlbedo = static_cast<ID3D12Resource*>(g.specular);
    ep.pInNormals = static_cast<ID3D12Resource*>(g.normals);
    ep.pInRoughness = static_cast<ID3D12Resource*>(g.normals);  // packed: read from the normals' w
    ep.InJitterOffsetX = F(gp, NVSDK_NGX_Parameter_Jitter_Offset_X, 0);
    ep.InJitterOffsetY = F(gp, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0);
    ep.InMVScaleX = F(gp, NVSDK_NGX_Parameter_MV_Scale_X, 1);
    ep.InMVScaleY = F(gp, NVSDK_NGX_Parameter_MV_Scale_Y, 1);
    ep.InRenderSubrectDimensions = {renderW, renderH};
    ep.InReset = (g_reset || U(gp, NVSDK_NGX_Parameter_Reset, 0)) ? 1 : 0;
    ep.InPreExposure = F(gp, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1);
    ep.InExposureScale = F(gp, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1);
    ep.pInWorldToViewMatrix = worldToView;
    ep.pInViewToClipMatrix = viewToClip;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSD_EXT(cl, g_handle, g_params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        if (++g_failures >= 30) {
            g_disabled = true;
            ReleaseFeature();
        }
        return Fail("Ray Reconstruction evaluation failed (" + ResultText(r) + ")");
    }
    g_failures = 0;
    g_reset = false;
    g_lastSeq = g.seq;
    if (g_evaluates++ == 0) Log("Ray Reconstruction evaluated (first time)");
    g_status = Format("running: %ux%u -> %ux%u", renderW, renderH, outW, outH);
    rt::SetRrWorking(true);
    return true;
}

void OnGameRelease() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ReleaseFeature();
    g_renderW = g_renderH = g_outW = g_outH = 0;
    g_disabled = false;
    rt::SetRrWorking(false);
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status + Format(" (%llu evaluations)", g_evaluates);
}

}  // namespace rr
