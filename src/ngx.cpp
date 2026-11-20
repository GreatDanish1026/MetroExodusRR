// The game's own DLSS calls.
//
// MetroExodus.exe links NVIDIA's NGX SDK statically: the SDK loads the driver's _nvngx.dll and looks up
// NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature / ReleaseFeature with GetProcAddress, through
// MetroExodus.exe's import table. Replacing that import-table entry lets the add-on hand the game small
// jump stubs instead of the driver's functions, so every DLSS call passes through here with all of
// its inputs (colour, depth, motion vectors, jitter...). Those are exactly the inputs frame
// generation needs.
//
// The stubs live in memory the add-on never frees: ReShade can unload the add-on while the game
// keeps the pointers it looked up. Each stub jumps through a slot that points at the add-on's
// wrapper while it is loaded and back at the driver's function after it unloads. A later load of
// the add-on in the same process finds the same stubs again (address kept in an environment
// variable with the process id).
//
// 0.0.1 only observes: arguments and results are passed through unchanged.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <nvsdk_ngx.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"

namespace ngx {
namespace {

enum Fn { kCreate, kEvaluate, kRelease, kFnCount };
const char* const kFnNames[kFnCount] = {"NVSDK_NGX_D3D12_CreateFeature", "NVSDK_NGX_D3D12_EvaluateFeature",
                                        "NVSDK_NGX_D3D12_ReleaseFeature"};

// Never freed; shared by every load of the add-on in this process.
struct Persistent {
    uint32_t magic;
    void* original[kFnCount];  // the driver's functions (0 until the game looks them up)
    void* target[kFnCount];    // where each stub jumps: the add-on's wrapper or the original
    alignas(16) uint8_t code[kFnCount][16];
};
constexpr uint32_t kMagic = 0x52525845;  // "EXRR"
Persistent* g_p = nullptr;

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_realGpa = nullptr;
void** g_iatSlot = nullptr;
std::atomic<bool> g_active{false};

// ReShade's IID for the native object behind one of its proxies (ReShade 6.8 source).
const GUID kUnwrapped = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

std::mutex g_mutex;  // the logging state below
std::set<std::string> g_lookups;
uint64_t g_creates = 0, g_evaluates = 0, g_releases = 0, g_resets = 0;
uint64_t g_evalsAtSummary = 0, g_presentsAtSummary = 0;
std::string g_lastSignature;
std::set<void*> g_seen[4];  // colour, output, depth, motion vectors
std::vector<std::string> g_jitters;
std::set<DWORD> g_threads;
float g_lastJitter[2] = {}, g_lastMvScale[2] = {};
bool g_proxyList = false;

Persistent* FindOrCreatePersistent() {
    wchar_t buf[64] = {};
    const DWORD pid = GetCurrentProcessId();
    if (GetEnvironmentVariableW(L"METROEXRR_STUBS", buf, 64)) {
        unsigned long envPid = 0;
        unsigned long long addr = 0;
        if (swscanf(buf, L"%lu:%llx", &envPid, &addr) == 2 && envPid == pid && addr) {
            auto* p = reinterpret_cast<Persistent*>(addr);
            if (p->magic == kMagic) return p;
        }
    }
    auto* p = static_cast<Persistent*>(VirtualAlloc(nullptr, sizeof(Persistent), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return nullptr;
    std::memset(p, 0, sizeof(*p));
    p->magic = kMagic;
    for (int i = 0; i < kFnCount; ++i) {
        uint8_t* c = p->code[i];
        c[0] = 0xff;  // jmp qword ptr [rip + disp32]
        c[1] = 0x25;
        const int32_t disp = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&p->target[i]) - (c + 6));
        std::memcpy(c + 2, &disp, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(*p));
    swprintf(buf, 64, L"%lu:%llx", pid, reinterpret_cast<unsigned long long>(p));
    SetEnvironmentVariableW(L"METROEXRR_STUBS", buf);
    return p;
}

// ---------------------------------------------------------------- parameter reading

ID3D12Resource* GetRes(const NVSDK_NGX_Parameter* p, const char* name) {
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success) return nullptr;
    return r;
}

// Description without the resource id, so ring-buffered inputs give the same text.
std::string SizeOnly(ID3D12Resource* r) {
    const std::string d = DescribeNative(r);
    const size_t space = d.find(' ');
    return d[0] == '#' && space != std::string::npos ? d.substr(space + 1) : d;
}

std::string F(const NVSDK_NGX_Parameter* p, const char* name) {
    float v = 0;
    if (p->Get(name, &v) != NVSDK_NGX_Result_Success) return "-";
    return Format("%g", v);
}

std::string U(const NVSDK_NGX_Parameter* p, const char* name) {
    unsigned int v = 0;
    if (p->Get(name, &v) != NVSDK_NGX_Result_Success) return "-";
    return Format("%u", v);
}

std::string Flags(unsigned int f) {
    std::string s = Format("0x%x", f);
    if (f & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) s += " HDR";
    if (f & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) s += " MVLowRes";
    if (f & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) s += " MVJittered";
    if (f & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) s += " DepthInverted";
    if (f & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening) s += " Sharpening";
    if (f & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) s += " AutoExposure";
    if (f & NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling) s += " AlphaUpscaling";
    return s;
}

const char* FeatureName(NVSDK_NGX_Feature id) {
    switch (id) {
        case NVSDK_NGX_Feature_SuperSampling: return "DLSS Super Resolution";
        case NVSDK_NGX_Feature_RayReconstruction: return "DLSS Ray Reconstruction";
        case NVSDK_NGX_Feature_FrameGeneration: return "DLSS Frame Generation";
        default: return "other";
    }
}

// NVSDK_NGX_Handle is opaque in the SDK headers; it starts with the feature's id.
unsigned HandleId(const NVSDK_NGX_Handle* h) { return h ? *reinterpret_cast<const unsigned*>(h) : 0; }

std::string ListInfo(ID3D12GraphicsCommandList* cl, uint64_t* native) {
    *native = reinterpret_cast<uint64_t>(cl);
    if (!cl) return "null";
    IUnknown* n = nullptr;
    if (SUCCEEDED(cl->QueryInterface(kUnwrapped, reinterpret_cast<void**>(&n))) && n) {
        *native = reinterpret_cast<uint64_t>(n);
        n->Release();
        return Format("ReShade proxy %p (native %p), type %d", cl, n, cl->GetType());
    }
    return Format("native %p, type %d", cl, cl->GetType());
}

// ---------------------------------------------------------------- wrappers

NVSDK_NGX_Result NVSDK_CONV WrapCreate(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p,
                                       NVSDK_NGX_Handle** out) {
    auto real = reinterpret_cast<decltype(&WrapCreate)>(g_p->original[kCreate]);
    const NVSDK_NGX_Result r = real(cl, id, p, out);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_creates;
    uint64_t native = 0;
    const std::string list = ListInfo(cl, &native);
    unsigned int flags = 0;
    if (p) p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    Log("NGX CreateFeature %d (%s) -> 0x%x, handle id %s, thread %lu, list %s", id, FeatureName(id), r,
        out && *out ? Format("%u", HandleId(*out)).c_str() : "-", GetCurrentThreadId(), list.c_str());
    if (p) {
        Log("  render %sx%s -> output %sx%s, PerfQuality %s, flags %s, node %s/%s", U(p, NVSDK_NGX_Parameter_Width).c_str(),
            U(p, NVSDK_NGX_Parameter_Height).c_str(), U(p, NVSDK_NGX_Parameter_OutWidth).c_str(),
            U(p, NVSDK_NGX_Parameter_OutHeight).c_str(), U(p, NVSDK_NGX_Parameter_PerfQualityValue).c_str(),
            Flags(flags).c_str(), U(p, NVSDK_NGX_Parameter_CreationNodeMask).c_str(),
            U(p, NVSDK_NGX_Parameter_VisibilityNodeMask).c_str());
        Log("  presets DLAA %s Quality %s Balanced %s Performance %s UltraPerf %s",
            U(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA).c_str(),
            U(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality).c_str(),
            U(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced).c_str(),
            U(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance).c_str(),
            U(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance).c_str());
    }
    g_lastSignature.clear();  // log the next evaluate in full
    return r;
}

NVSDK_NGX_Result NVSDK_CONV WrapEvaluate(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p,
                                         PFN_NVSDK_NGX_ProgressCallback cb) {
    uint64_t native = 0;
    std::string list;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_evaluates;
        g_threads.insert(GetCurrentThreadId());
        list = ListInfo(cl, &native);
        g_proxyList = native != reinterpret_cast<uint64_t>(cl);
        if (p) {
            static const char* const kRes[4] = {NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Output,
                                                NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors};
            ID3D12Resource* res[4];
            for (int i = 0; i < 4; ++i) res[i] = GetRes(p, kRes[i]);
            p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &g_lastJitter[0]);
            p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &g_lastJitter[1]);
            p->Get(NVSDK_NGX_Parameter_MV_Scale_X, &g_lastMvScale[0]);
            p->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &g_lastMvScale[1]);
            unsigned int reset = 0;
            p->Get(NVSDK_NGX_Parameter_Reset, &reset);
            if (reset) ++g_resets;
            if (g_jitters.size() < 32) g_jitters.push_back(Format("(%.3f,%.3f)", g_lastJitter[0], g_lastJitter[1]));
            if (g_jitters.size() == 32) {
                std::string s;
                for (const std::string& j : g_jitters) s += " " + j;
                Log("first 32 jitters:%s", s.c_str());
                g_jitters.push_back("");  // logged once
            }

            const std::string sizes = SizeOnly(res[0]) + " / " + SizeOnly(res[1]) + " / " + SizeOnly(res[2]) + " / " + SizeOnly(res[3]);
            const std::string signature = sizes + " render " + U(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) +
                                          "x" + U(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height) + " mvscale " +
                                          F(p, NVSDK_NGX_Parameter_MV_Scale_X) + "," + F(p, NVSDK_NGX_Parameter_MV_Scale_Y);
            if (signature != g_lastSignature || g_evaluates <= 3) {
                g_lastSignature = signature;
                Log("NGX EvaluateFeature #%llu, handle %s, present %llu, thread %lu, list %s, callback %p", g_evaluates,
                    h ? Format("%u", HandleId(h)).c_str() : "-", PresentCount(), GetCurrentThreadId(), list.c_str(), cb);
                Log("  colour %s", DescribeNative(res[0]).c_str());
                Log("  output %s", DescribeNative(res[1]).c_str());
                Log("  depth  %s", DescribeNative(res[2]).c_str());
                Log("  mv     %s", DescribeNative(res[3]).c_str());
                ID3D12Resource* extra[4] = {GetRes(p, NVSDK_NGX_Parameter_TransparencyMask), GetRes(p, NVSDK_NGX_Parameter_ExposureTexture),
                                            GetRes(p, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask),
                                            GetRes(p, NVSDK_NGX_Parameter_MotionVectors3D)};
                Log("  transparency %s, exposure %s, bias mask %s, mv3d %s", DescribeNative(extra[0]).c_str(),
                    DescribeNative(extra[1]).c_str(), DescribeNative(extra[2]).c_str(), DescribeNative(extra[3]).c_str());
                Log("  render subrect %sx%s, colour base %s,%s depth base %s,%s mv base %s,%s output base %s,%s",
                    U(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y).c_str());
                Log("  jitter %s,%s mvscale %s,%s reset %u sharpness %s pre-exposure %s exposure scale %s invert indicator %s/%s",
                    F(p, NVSDK_NGX_Parameter_Jitter_Offset_X).c_str(), F(p, NVSDK_NGX_Parameter_Jitter_Offset_Y).c_str(),
                    F(p, NVSDK_NGX_Parameter_MV_Scale_X).c_str(), F(p, NVSDK_NGX_Parameter_MV_Scale_Y).c_str(), reset,
                    F(p, NVSDK_NGX_Parameter_Sharpness).c_str(), F(p, NVSDK_NGX_Parameter_DLSS_Pre_Exposure).c_str(),
                    F(p, NVSDK_NGX_Parameter_DLSS_Exposure_Scale).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis).c_str(),
                    U(p, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis).c_str());
            }
            // Ring buffers: note each new texture behind an input (up to 8 per input).
            static const char* const kShort[4] = {"colour", "output", "depth", "mv"};
            for (int i = 0; i < 4; ++i) {
                if (!res[i] || g_seen[i].count(res[i]) || g_seen[i].size() >= 8) continue;
                g_seen[i].insert(res[i]);
                if (g_evaluates > 1)
                    Log("  new %s texture at evaluate #%llu: %s (%zu seen)", kShort[i], g_evaluates, DescribeNative(res[i]).c_str(),
                        g_seen[i].size());
            }
            CaptureNote(native, Format("NGX EVALUATE DLSS: colour %s output %s depth %s mv %s jitter %.4f,%.4f",
                                       DescribeNative(res[0]).c_str(), DescribeNative(res[1]).c_str(), DescribeNative(res[2]).c_str(),
                                       DescribeNative(res[3]).c_str(), g_lastJitter[0], g_lastJitter[1]));
        }
    }
    auto real = reinterpret_cast<decltype(&WrapEvaluate)>(g_p->original[kEvaluate]);
    const NVSDK_NGX_Result r = real(cl, h, p, cb);
    if (NVSDK_NGX_FAILED(r)) Log("NGX EvaluateFeature failed: 0x%x", r);
    return r;
}

NVSDK_NGX_Result NVSDK_CONV WrapRelease(NVSDK_NGX_Handle* h) {
    const unsigned id = HandleId(h);
    auto real = reinterpret_cast<decltype(&WrapRelease)>(g_p->original[kRelease]);
    const NVSDK_NGX_Result r = real(h);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_releases;
    Log("NGX ReleaseFeature handle %u -> 0x%x (after %llu evaluates)", id, r, g_evaluates);
    return r;
}

void* const kWrappers[kFnCount] = {reinterpret_cast<void*>(&WrapCreate), reinterpret_cast<void*>(&WrapEvaluate),
                                   reinterpret_cast<void*>(&WrapRelease)};

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC r = g_realGpa(module, name);
    if (!r || IS_INTRESOURCE(name) || std::strncmp(name, "NVSDK_NGX_", 10) != 0) return r;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* file = std::strrchr(path, '\\');
    file = file ? file + 1 : path;
    for (int i = 0; i < kFnCount; ++i) {
        if (std::strcmp(name, kFnNames[i]) != 0) continue;
        g_p->original[i] = reinterpret_cast<void*>(r);
        g_p->target[i] = g_active ? kWrappers[i] : reinterpret_cast<void*>(r);
        Log("game looked up %s in %s: %p, wrapped", name, file, r);
        return reinterpret_cast<FARPROC>(&g_p->code[i][0]);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lookups.insert(name).second) Log("game looked up %s in %s: %p", name, file, r);
    return r;
}

// MetroExodus.exe's import-table slot for KERNEL32!GetProcAddress.
void** FindIatSlot() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), "KERNEL32.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<void**>(base + imp->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) || !imp->OriginalFirstThunk) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(byName->Name), "GetProcAddress") == 0) return &slots[i];
        }
    }
    return nullptr;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

void Install() {
    g_p = FindOrCreatePersistent();
    if (!g_p) {
        Log("couldn't allocate the NGX stubs");
        return;
    }
    // A previous load in this process may already have handed the game stubs: point them back at
    // this load's wrappers.
    for (int i = 0; i < kFnCount; ++i) {
        if (g_p->original[i]) {
            g_p->target[i] = kWrappers[i];
            Log("%s was looked up before this load; stub re-pointed at the wrapper", kFnNames[i]);
        }
    }
    g_active = true;
    g_iatSlot = FindIatSlot();
    if (!g_iatSlot) {
        Log("GetProcAddress not found in MetroExodus.exe's import table: DLSS calls can't be observed");
        return;
    }
    g_realGpa = reinterpret_cast<GetProcAddressFn>(*g_iatSlot);
    if (!WriteSlot(g_iatSlot, reinterpret_cast<void*>(&HookGetProcAddress))) {
        Log("couldn't patch MetroExodus.exe's GetProcAddress import (error %lu)", GetLastError());
        g_iatSlot = nullptr;
        return;
    }
    Log("hooked MetroExodus.exe's GetProcAddress import (was %p)", g_realGpa);
}

void Uninstall() {
    g_active = false;
    if (g_p)
        for (int i = 0; i < kFnCount; ++i)
            if (g_p->original[i]) g_p->target[i] = g_p->original[i];
    if (g_iatSlot && *g_iatSlot == reinterpret_cast<void*>(&HookGetProcAddress)) WriteSlot(g_iatSlot, reinterpret_cast<void*>(g_realGpa));
    g_iatSlot = nullptr;
    Log("NGX hooks removed (%llu creates, %llu evaluates, %llu releases)", g_creates, g_evaluates, g_releases);
}

void OnPresent() {
    const uint64_t presents = PresentCount();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (presents - g_presentsAtSummary < 1200) return;
    std::string threads;
    for (DWORD t : g_threads) threads += Format(" %lu", t);
    Log("summary: %llu DLSS evaluates in the last %llu presents (total %llu, %llu resets), jitter %.3f,%.3f mvscale %g,%g, "
        "threads%s, %s command lists, textures seen %zu/%zu/%zu/%zu",
        g_evaluates - g_evalsAtSummary, presents - g_presentsAtSummary, g_evaluates, g_resets, g_lastJitter[0], g_lastJitter[1],
        g_lastMvScale[0], g_lastMvScale[1], threads.empty() ? " none" : threads.c_str(), g_proxyList ? "proxy" : "native",
        g_seen[0].size(), g_seen[1].size(), g_seen[2].size(), g_seen[3].size());
    g_presentsAtSummary = presents;
    g_evalsAtSummary = g_evaluates;
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_iatSlot) return "GetProcAddress hook not installed (see MetroExodusRR.log)";
    if (!g_p->original[kEvaluate]) return "waiting for the game to look up DLSS (NGX)";
    return Format("DLSS evaluates: %llu (features created %llu), jitter %.3f, %.3f", g_evaluates, g_creates, g_lastJitter[0],
                  g_lastJitter[1]);
}

}  // namespace ngx
