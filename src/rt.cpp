// The game's ray-tracing passes (0.0.2 experiments).
//
// Metro Exodus EE traces its RT global illumination and reflections inline (RayQuery) in compute
// shaders and denoises the GI with a chain of compute passes (FINDINGS.md, "0.0.1 results").
// Ray Reconstruction needs that chain switched off, so this file:
// - follows each command list's compute state (pipeline, descriptor tables, root constant buffers)
//   and resolves which textures each known pass reads (t#) and writes (u#): the binding map, logged
//   once per pass and again on F11;
// - F11 also writes each known pass's root constant buffer to MetroExodusRR-cb-<pass>.bin;
// - live experiments in the overlay: skip denoiser passes, replace the final blur with a plain copy,
//   clear the temporal history before the temporal pass;
// - a texture viewer: any input or output of a known pass, copied at present and shown in the
//   overlay.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "gi_center_cs.h"    // kGiCenterCs, built from gi_center.hlsl
#include "count_reset_cs.h"  // kCountResetCs, built from count_reset.hlsl
#include "guide_cs.h"        // kGuideCs, built from guide.hlsl
#include "common.hpp"
#include "version.h"

using namespace reshade::api;

namespace rt {
namespace {

// ---------------------------------------------------------------- known passes (ids from 0.0.1)

enum PassId { kGiPrep, kGiTrace, kGiShade, kGiTemporal, kGiMips, kGiGapFix, kGiBlur, kComposite, kReflTrace, kRtAfter, kPassCount };

struct PassInfo {
    uint32_t cs;
    const char* name;  // also the cb dump file name
    bool skippable;    // offered as "skip" in the overlay
};
const PassInfo kPasses[kPassCount] = {
    {0xa48f6785, "gi-prep", false},          {0xb425d82a, "gi-trace", false},
    {0x1fce2042, "gi-hit-shading", false},   {0xb83925ed, "gi-temporal", true},
    {0x648ac480, "gi-mips", true},           {0x1e664fee, "gi-gapfix", true},
    {0x66e47af2, "gi-spatial-blur", true},   {0x56190203, "rt-composite", false},
    {0x57d4ae57, "reflection-trace", false}, {0x576e2cf3, "rt-after-composite", false},
};

int PassFromCs(uint32_t cs) {
    for (int i = 0; i < kPassCount; ++i)
        if (kPasses[i].cs == cs) return i;
    return -1;
}

// ---------------------------------------------------------------- per command list compute state

struct ListState {
    uint32_t cs = 0;
    pipeline pipe = {0};
    pipeline_layout layout = {0};
    descriptor_table tables[8] = {};
    buffer_range cbv[8] = {};
};
std::mutex g_listMutex;
std::unordered_map<uint64_t, ListState> g_listState;

// ---------------------------------------------------------------- binding maps

struct Slot {
    char reg[8];  // "t3", "u0"
    resource res;
};
struct BindingMap {
    std::vector<Slot> slots;
    uint64_t frame = 0;
    uint32_t dispatches = 0;  // this frame
    uint32_t gx = 0, gy = 0, gz = 0;
};

std::mutex g_mapMutex;  // g_maps, g_state, g_seenPass, g_views, g_tracked
BindingMap g_maps[kPassCount];
bool g_seenPass[kPassCount] = {};
std::unordered_map<uint64_t, resource_usage> g_state;  // last barrier target of resources in a map
std::set<uint64_t> g_tracked;                          // resources whose barriers are followed
std::unordered_map<uint64_t, resource_view> g_uavs;    // our UAVs (history clear, centre pass)
std::unordered_map<uint64_t, resource_view> g_srvs;    // our SRVs (centre pass)

// Experiments (overlay, live)
std::atomic<bool> g_skip[kPassCount] = {};
std::atomic<bool> g_blurCopy{false};
std::atomic<bool> g_clearHistory{false};  // breaks the GI (count kept, history black): kept for reference
std::atomic<bool> g_resetCount{false};    // frame count of the temporal pass's t3 capped
std::atomic<int> g_countCap{3};           // the cap (0 = current frame only, 31 = game); 3 keeps brightness (run8)
// All of the GI denoiser off at once: history cleared, mips + gap fix skipped, blur -> copy.
std::atomic<bool> g_denoiserOff{false};
std::atomic<bool> g_measureRequested{false};
std::atomic<bool> g_dumpRequested{false};

std::atomic<bool> g_rrWanted{true};    // overlay: Ray Reconstruction instead of the game's DLSS
std::atomic<int> g_rrPreset{0};        // RR model hint: 0 NVIDIA default, 4 D, 5 E, 6 F
std::atomic<bool> g_rrWorking{false};  // rr.cpp: RR ran last frame
// RR needs the GI noisy: the denoiser goes off while RR runs.
bool DenoiserOff() { return g_denoiserOff.load() || (g_rrWanted.load() && g_rrWorking.load()); }
bool ClearHistoryOn() { return g_clearHistory.load(); }
bool ResetCountOn() { return g_resetCount.load() || DenoiserOff(); }
bool BlurCopyOn() { return g_blurCopy.load() || DenoiserOff(); }
bool SkipOn(int pass) { return g_skip[pass].load() || (DenoiserOff() && (pass == kGiMips || pass == kGiGapFix)); }
std::atomic<bool> g_dumpPending[kPassCount] = {};
std::atomic<uint64_t> g_actions{0};  // experiment actions done (for the overlay)

const char* RegLetter(descriptor_type t) {
    switch (t) {
        case descriptor_type::shader_resource_view:
        case descriptor_type::buffer_shader_resource_view: return "t";
        case descriptor_type::unordered_access_view:
        case descriptor_type::buffer_unordered_access_view: return "u";
        default: return nullptr;
    }
}

// Every SRV/UAV in the tables bound for the current dispatch.
std::vector<Slot> Resolve(device* dev, const ListState& st) {
    std::vector<Slot> out;
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt || !st.layout.handle) return out;
    for (uint32_t param = 0; param < 8; ++param) {
        if (!st.tables[param].handle) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
        for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
            const descriptor_range& range = p.descriptor_table.ranges[r];
            const char* letter = RegLetter(range.type);
            if (!letter || range.dx_register_space != 0) continue;
            const uint32_t n = range.count > 24 ? 24 : range.count;
            for (uint32_t k = 0; k < n; ++k) {
                descriptor_heap heap = {0};
                uint32_t offset = 0;
                dev->get_descriptor_heap_offset(st.tables[param], range.binding + k, 0, &heap, &offset);
                const resource_view v = dt->get_resource_view(heap, offset);
                if (!v.handle) continue;
                Slot s = {};
                snprintf(s.reg, sizeof(s.reg), "%s%u", letter, range.dx_register_index + k);
                s.res = dev->get_resource_from_view(v);
                if (s.res.handle) out.push_back(s);
            }
        }
    }
    return out;
}

resource Find(const BindingMap& m, const char* reg) {
    for (const Slot& s : m.slots)
        if (strcmp(s.reg, reg) == 0) return s.res;
    return {0};
}

std::string Desc(resource r) { return DescribeNative(reinterpret_cast<void*>(r.handle)); }

void LogMap(int pass, const BindingMap& m) {
    std::string s = Format("pass %s (cs%08x) dispatch %u x %u x %u:", kPasses[pass].name, kPasses[pass].cs, m.gx, m.gy, m.gz);
    Log("%s", s.c_str());
    for (const Slot& sl : m.slots) Log("    %-4s %s", sl.reg, Desc(sl.res).c_str());
}

// Root constant buffer of the dispatch, written to a file next to the exe.
void DumpConstants(device* dev, int pass, const ListState& st) {
    for (uint32_t param = 0; param < 8; ++param) {
        const buffer_range& b = st.cbv[param];
        if (!b.buffer.handle) continue;
        const resource_desc bd = dev->get_resource_desc(b.buffer);
        if (bd.type != resource_type::buffer || b.offset >= bd.buffer.size) continue;
        uint64_t size = bd.buffer.size - b.offset;
        if (size > 65536) size = 65536;
        void* data = nullptr;
        if (!dev->map_buffer_region(b.buffer, b.offset, size, map_access::read_only, &data) || !data) {
            Log("cb dump %s param %u: map failed", kPasses[pass].name, param);
            continue;
        }
        wchar_t name[96];
        swprintf(name, 96, L"%s-cb-%hs-p%u.bin", MD_PRODUCT_W, kPasses[pass].name, param);
        if (FILE* f = _wfopen((ExeDir() + name).c_str(), L"wb")) {
            fwrite(data, 1, static_cast<size_t>(size), f);
            fclose(f);
        }
        dev->unmap_buffer_region(b.buffer);
        Log("cb dump %s param %u: %llu bytes at +%llu", kPasses[pass].name, param, size, b.offset);
    }
}

resource_usage StateOf(resource r) {  // g_mapMutex held
    auto it = g_state.find(r.handle);
    return it == g_state.end() ? resource_usage::undefined : it->second;
}

// Copies mip 0 of src into dst on the game's command list, restoring both states.
bool CopyTex(command_list* cl, resource src, resource_usage srcState, resource dst, resource_usage dstState) {
    device* dev = cl->get_device();
    const resource_desc a = dev->get_resource_desc(src), b = dev->get_resource_desc(dst);
    if (a.texture.width != b.texture.width || a.texture.height != b.texture.height || a.texture.format != b.texture.format) return false;
    if (srcState == resource_usage::undefined || dstState == resource_usage::undefined) return false;
    resource rs[2] = {src, dst};
    resource_usage from[2] = {srcState, dstState}, to[2] = {resource_usage::copy_source, resource_usage::copy_dest};
    cl->barrier(2, rs, from, to);
    cl->copy_texture_region(src, 0, nullptr, dst, 0, nullptr);
    cl->barrier(2, rs, to, from);
    return true;
}

// Our view of mip 0 of a game texture (cached until the texture is destroyed).
resource_view CachedView(device* dev, resource r, bool uav, format fmt = format::unknown) {
    auto& views = uav ? g_uavs : g_srvs;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = views.find(r.handle);
        if (it != views.end()) return it->second;
    }
    const resource_desc d = dev->get_resource_desc(r);
    if (uav && (d.usage & resource_usage::unordered_access) == 0) return {0};
    resource_view v = {0};
    const resource_view_desc vd(resource_view_type::texture_2d, fmt != format::unknown ? fmt : d.texture.format, 0, 1, 0, 1);
    if (!dev->create_resource_view(r, uav ? resource_usage::unordered_access : resource_usage::shader_resource, vd, &v)) return {0};
    std::lock_guard<std::mutex> lock(g_mapMutex);
    views[r.handle] = v;
    return v;
}

// ---------------------------------------------------------------- centre-only GI output pass

pipeline_layout g_centreLayout = {0};
pipeline g_centrePipe = {0};
bool g_centreFailed = false;
std::atomic<uint64_t> g_centreRuns{0};
std::string g_centreError;

bool CreateCentrePipeline(device* dev) {
    if (g_centrePipe.handle) return true;
    if (g_centreFailed) return false;
    descriptor_range srv = {};
    srv.count = 2;
    srv.visibility = shader_stage::all_compute;
    srv.type = descriptor_type::shader_resource_view;
    descriptor_range uav = {};
    uav.count = 2;
    uav.visibility = shader_stage::all_compute;
    uav.type = descriptor_type::unordered_access_view;
    const pipeline_layout_param params[2] = {pipeline_layout_param(srv), pipeline_layout_param(uav)};
    shader_desc cs = {};
    cs.code = kGiCenterCs;
    cs.code_size = sizeof(kGiCenterCs);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline_layout(2, params, &g_centreLayout) || !dev->create_pipeline(g_centreLayout, 1, &sub, &g_centrePipe)) {
        g_centreFailed = true;
        g_centreError = "couldn't create the centre-only GI shader";
        Log("%s", g_centreError.c_str());
        return false;
    }
    Log("centre-only GI shader created");
    return true;
}

// Puts the game's compute bindings back: an empty bind makes ReShade set the game's root
// signature (and descriptor heaps) again.
void RestoreCompute(command_list* cl, const ListState& st) {
    if (st.layout.handle) {
        cl->bind_descriptor_tables(shader_stage::all_compute, st.layout, 0, 0, nullptr);
        for (uint32_t i = 0; i < 8; ++i)
            if (st.tables[i].handle) cl->bind_descriptor_tables(shader_stage::all_compute, st.layout, i, 1, &st.tables[i]);
        for (uint32_t i = 0; i < 8; ++i) {
            if (!st.cbv[i].buffer.handle) continue;
            descriptor_table_update u = {};
            u.count = 1;
            u.type = descriptor_type::constant_buffer;
            u.descriptors = &st.cbv[i];
            cl->push_descriptors(shader_stage::all_compute, st.layout, i, u);
        }
    }
    if (st.pipe.handle) cl->bind_pipeline(pipeline_stage::all_compute, st.pipe);
}

void CentreFail(const std::string& why) {
    if (g_centreError != why) Log("centre-only GI pass not run: %s", why.c_str());
    g_centreError = why;
}

bool RunCentre(command_list* cl, const ListState& st, const BindingMap& m) {
    device* dev = cl->get_device();
    if (!CreateCentrePipeline(dev)) return false;
    const resource inA = Find(m, "t3"), inB = Find(m, "t4"), outA = Find(m, "u0"), outB = Find(m, "u1");
    if (!inA.handle || !inB.handle || !outA.handle || !outB.handle) return CentreFail("blur bindings missing"), false;
    resource_usage s[4];
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        s[0] = StateOf(inA), s[1] = StateOf(inB), s[2] = StateOf(outA), s[3] = StateOf(outB);
    }
    // The game has just moved the inputs to non-pixel SRV and the outputs to UAV for its blur.
    if (s[0] != resource_usage::shader_resource_non_pixel || s[1] != resource_usage::shader_resource_non_pixel ||
        s[2] != resource_usage::unordered_access || s[3] != resource_usage::unordered_access)
        return CentreFail(Format("unexpected states %x %x %x %x", (unsigned)s[0], (unsigned)s[1], (unsigned)s[2], (unsigned)s[3])), false;
    const resource_view srvs[2] = {CachedView(dev, inA, false), CachedView(dev, inB, false)};
    const resource_view uavs[2] = {CachedView(dev, outA, true), CachedView(dev, outB, true)};
    if (!srvs[0].handle || !srvs[1].handle || !uavs[0].handle || !uavs[1].handle) return CentreFail("views"), false;
    const resource_desc d = dev->get_resource_desc(outA);
    cl->bind_pipeline(pipeline_stage::all_compute, g_centrePipe);
    descriptor_table_update u = {};
    u.count = 2;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = srvs;
    cl->push_descriptors(shader_stage::all_compute, g_centreLayout, 0, u);
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = uavs;
    cl->push_descriptors(shader_stage::all_compute, g_centreLayout, 1, u);
    cl->dispatch((d.texture.width + 7) / 8, (d.texture.height + 7) / 8, 1);
    RestoreCompute(cl, st);
    if (g_centreRuns++ == 0) Log("centre-only GI pass ran (first time)");
    g_centreError.clear();
    return true;
}

// ---------------------------------------------------------------- frame count reset pass

pipeline_layout g_resetLayout = {0};
pipeline g_resetPipe = {0};
bool g_resetFailed = false;
std::atomic<uint64_t> g_resetRuns{0};
std::string g_resetError;

void ResetFail(const std::string& why) {
    if (g_resetError != why) Log("frame count reset not run: %s", why.c_str());
    g_resetError = why;
}

bool RunCountReset(command_list* cl, const ListState& st, const BindingMap& m) {
    device* dev = cl->get_device();
    if (!g_resetPipe.handle && !g_resetFailed) {
        descriptor_range uav = {};
        uav.count = 1;
        uav.visibility = shader_stage::all_compute;
        uav.type = descriptor_type::unordered_access_view;
        constant_range c = {};
        c.count = 1;
        c.visibility = shader_stage::all_compute;
        const pipeline_layout_param params[2] = {pipeline_layout_param(uav), pipeline_layout_param(c)};
        shader_desc cs = {};
        cs.code = kCountResetCs;
        cs.code_size = sizeof(kCountResetCs);
        pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
        if (!dev->create_pipeline_layout(2, params, &g_resetLayout) || !dev->create_pipeline(g_resetLayout, 1, &sub, &g_resetPipe)) {
            g_resetFailed = true;
            ResetFail("couldn't create the shader");
        } else {
            Log("frame count reset shader created");
        }
    }
    if (!g_resetPipe.handle) return false;
    const resource r = Find(m, "t3");
    if (!r.handle) return ResetFail("temporal t3 missing"), false;
    const resource_desc d = dev->get_resource_desc(r);
    if (d.texture.format != format::r32_float) return ResetFail("temporal t3 isn't r32f"), false;
    resource_usage state;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        state = StateOf(r);
    }
    if (state != resource_usage::shader_resource_non_pixel) return ResetFail(Format("t3 state %x", (unsigned)state)), false;
    const resource_view uav = CachedView(dev, r, true);
    if (!uav.handle) return ResetFail("no UAV for t3"), false;
    cl->barrier(r, state, resource_usage::unordered_access);
    cl->bind_pipeline(pipeline_stage::all_compute, g_resetPipe);
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = &uav;
    cl->push_descriptors(shader_stage::all_compute, g_resetLayout, 0, u);
    const uint32_t cap = static_cast<uint32_t>(g_countCap.load());
    cl->push_constants(shader_stage::all_compute, g_resetLayout, 1, 0, 1, &cap);
    cl->dispatch((d.texture.width + 7) / 8, (d.texture.height + 7) / 8, 1);
    cl->barrier(r, resource_usage::unordered_access, state);
    RestoreCompute(cl, st);
    if (g_resetRuns++ == 0) Log("frame count reset ran (first time)");
    g_resetError.clear();
    return true;
}

// ---------------------------------------------------------------- Ray Reconstruction guides

pipeline_layout g_guideLayout = {0};
pipeline g_guidePipe = {0};
bool g_guideFailed = false;
resource g_guideTex[3] = {};
resource_view g_guideUav[3] = {};
uint32_t g_guideW = 0, g_guideH = 0;
std::vector<resource> g_guideRetired;  // old-size guides, destroyed with the device
std::mutex g_guideMutex;               // g_guides
rt::Guides g_guides = {};
bool g_guidesValid = false;
std::atomic<uint64_t> g_guideRuns{0};
std::string g_guideError;

void GuideFail(const std::string& why) {
    if (g_guideError != why) Log("RR guides not made: %s", why.c_str());
    g_guideError = why;
}

// <count> floats at byte <offset> of the dispatch's root constant buffer <param>.
bool ReadCb(device* dev, const ListState& st, uint32_t param, uint32_t offset, float* out, uint32_t count) {
    const buffer_range& b = st.cbv[param];
    if (!b.buffer.handle) return false;
    const resource_desc bd = dev->get_resource_desc(b.buffer);
    if (bd.type != resource_type::buffer || b.offset + offset + count * 4ull > bd.buffer.size) return false;
    void* data = nullptr;
    if (!dev->map_buffer_region(b.buffer, b.offset + offset, count * 4, map_access::read_only, &data) || !data) return false;
    std::memcpy(out, data, count * 4);
    dev->unmap_buffer_region(b.buffer);
    return true;
}

bool EnsureGuideTextures(device* dev, uint32_t w, uint32_t h) {
    if (g_guideTex[0].handle && g_guideW == w && g_guideH == h) return true;
    for (int i = 0; i < 3; ++i) {
        if (g_guideUav[i].handle) dev->destroy_resource_view(g_guideUav[i]);
        if (g_guideTex[i].handle) g_guideRetired.push_back(g_guideTex[i]);
        g_guideTex[i] = {0};
        g_guideUav[i] = {0};
    }
    for (int i = 0; i < 3; ++i) {
        const resource_desc d(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_,
                              resource_usage::unordered_access | resource_usage::shader_resource);
        if (!dev->create_resource(d, nullptr, resource_usage::shader_resource, &g_guideTex[i]) ||
            !dev->create_resource_view(g_guideTex[i], resource_usage::unordered_access,
                                       resource_view_desc(format::r16g16b16a16_float), &g_guideUav[i]))
            return false;
    }
    g_guideW = w, g_guideH = h;
    Log("RR guide textures created: %ux%u", w, h);
    return true;
}

bool IsSrvState(resource_usage u) { return (static_cast<uint32_t>(u) & static_cast<uint32_t>(resource_usage::shader_resource_non_pixel)) != 0; }

// At the RT composite dispatch: the G-buffer and depth are bound as its t2/t3/t4/t8, and its root
// CBV holds the camera (registers 7-9 world->view 3x4, 10-12 view->world rotation, 13-16 projection
// with jitter in [0][2]/[1][2]; column vectors).
bool RunGuides(command_list* cl, const ListState& st, const BindingMap& m) {
    device* dev = cl->get_device();
    if (!g_guidePipe.handle && !g_guideFailed) {
        descriptor_range srv = {};
        srv.count = 4;
        srv.visibility = shader_stage::all_compute;
        srv.type = descriptor_type::shader_resource_view;
        descriptor_range uav = {};
        uav.count = 3;
        uav.visibility = shader_stage::all_compute;
        uav.type = descriptor_type::unordered_access_view;
        constant_range c = {};
        c.count = 16;
        c.visibility = shader_stage::all_compute;
        const pipeline_layout_param params[3] = {pipeline_layout_param(srv), pipeline_layout_param(uav), pipeline_layout_param(c)};
        shader_desc cs = {};
        cs.code = kGuideCs;
        cs.code_size = sizeof(kGuideCs);
        pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
        if (!dev->create_pipeline_layout(3, params, &g_guideLayout) || !dev->create_pipeline(g_guideLayout, 1, &sub, &g_guidePipe)) {
            g_guideFailed = true;
            GuideFail("couldn't create the guide shader");
        } else {
            Log("RR guide shader created");
        }
    }
    if (!g_guidePipe.handle) return false;
    const resource g0 = Find(m, "t2"), g1 = Find(m, "t3"), g2 = Find(m, "t4"), depth = Find(m, "t8");
    if (!g0.handle || !g1.handle || !g2.handle || !depth.handle) return GuideFail("composite bindings missing"), false;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        const resource_usage s[4] = {StateOf(g0), StateOf(g1), StateOf(g2), StateOf(depth)};
        for (int i = 0; i < 4; ++i)
            if (!IsSrvState(s[i])) return GuideFail(Format("G-buffer state %x", static_cast<unsigned>(s[i]))), false;
    }
    float c[40];  // registers 7..16
    if (!ReadCb(dev, st, 1, 7 * 16, c, 40)) return GuideFail("couldn't read the composite's constants"), false;
    const resource_desc gd = dev->get_resource_desc(g0);
    const uint32_t w = gd.texture.width, h = gd.texture.height;
    if (!EnsureGuideTextures(dev, w, h)) return GuideFail("couldn't create the guide textures"), false;
    const resource_view srvs[4] = {CachedView(dev, g0, false), CachedView(dev, g1, false), CachedView(dev, g2, false),
                                   CachedView(dev, depth, false, format::r32_float_x8_uint)};
    for (const resource_view& v : srvs)
        if (!v.handle) return GuideFail("G-buffer views"), false;

    const float* A = c;       // world -> view, rows 7-9 (xyz rotation, w translation)
    const float* R = c + 12;  // view -> world rotation, rows 10-12
    const float* P = c + 24;  // projection, rows 13-16
    float pc[16] = {R[0], R[1], R[2], 0, R[4], R[5], R[6], 0, R[8], R[9], R[10], 0,
                    1.0f / P[0], 1.0f / P[5], 1.0f / w, 1.0f / h};

    const resource_usage from[3] = {resource_usage::shader_resource, resource_usage::shader_resource, resource_usage::shader_resource};
    const resource_usage to[3] = {resource_usage::unordered_access, resource_usage::unordered_access, resource_usage::unordered_access};
    cl->barrier(3, g_guideTex, from, to);
    cl->bind_pipeline(pipeline_stage::all_compute, g_guidePipe);
    descriptor_table_update u = {};
    u.count = 4;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = srvs;
    cl->push_descriptors(shader_stage::all_compute, g_guideLayout, 0, u);
    u.count = 3;
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = g_guideUav;
    cl->push_descriptors(shader_stage::all_compute, g_guideLayout, 1, u);
    cl->push_constants(shader_stage::all_compute, g_guideLayout, 2, 0, 16, pc);
    cl->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl->barrier(3, g_guideTex, to, from);
    RestoreCompute(cl, st);

    // NGX matrices: row-major, row vectors (v_row * M).
    rt::Guides gd2 = {};
    // world -> view: view = A_rot * world + t  =>  M[i][j] = A[j][i], translation in row 3.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) gd2.worldToView[i * 4 + j] = A[j * 4 + i];
    for (int j = 0; j < 3; ++j) gd2.worldToView[12 + j] = A[j * 4 + 3];
    gd2.worldToView[15] = 1;
    // view -> clip: the transpose of the projection, without the jitter terms.
    float Pn[16];
    std::memcpy(Pn, P, sizeof(Pn));
    Pn[2] = 0, Pn[6] = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) gd2.viewToClip[i * 4 + j] = Pn[j * 4 + i];
    gd2.diffuse = reinterpret_cast<void*>(g_guideTex[0].handle);
    gd2.specular = reinterpret_cast<void*>(g_guideTex[1].handle);
    gd2.normals = reinterpret_cast<void*>(g_guideTex[2].handle);
    {
        std::lock_guard<std::mutex> lock(g_guideMutex);
        gd2.seq = g_guides.seq + 1;
        g_guides = gd2;
        g_guidesValid = true;
    }
    if (g_guideRuns++ == 0) {
        Log("RR guides made (first time): %ux%u, P00 %.4f P11 %.4f, camera at %.2f %.2f %.2f", w, h, P[0], P[5], R[3], R[7], R[11]);
    }
    g_guideError.clear();
    return true;
}

bool ClearTex(command_list* cl, resource r, resource_usage state) {
    if (state == resource_usage::undefined) return false;
    device* dev = cl->get_device();
    resource_view uav = {0};
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_uavs.find(r.handle);
        if (it != g_uavs.end()) uav = it->second;
    }
    if (!uav.handle) {
        const resource_desc d = dev->get_resource_desc(r);
        if ((d.usage & resource_usage::unordered_access) == 0) return false;
        resource_view_desc vd(resource_view_type::texture_2d, d.texture.format, 0, 1, 0, 1);
        if (!dev->create_resource_view(r, resource_usage::unordered_access, vd, &uav)) return false;
        std::lock_guard<std::mutex> lock(g_mapMutex);
        g_uavs[r.handle] = uav;
    }
    const float zero[4] = {0, 0, 0, 0};
    if (state != resource_usage::unordered_access) cl->barrier(r, state, resource_usage::unordered_access);
    cl->clear_unordered_access_view_float(uav, zero);
    if (state != resource_usage::unordered_access) cl->barrier(r, resource_usage::unordered_access, state);
    else cl->barrier(r, resource_usage::unordered_access, resource_usage::unordered_access);  // UAV barrier
    return true;
}

// ---------------------------------------------------------------- events

void OnReset(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_listState[cl->get_native()] = {};
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    if ((stages & pipeline_stage::compute_shader) == 0) return;
    const uint32_t cs = ComputeShaderId(pipe.handle);
    std::lock_guard<std::mutex> lock(g_listMutex);
    ListState& st = g_listState[cl->get_native()];
    st.cs = cs;
    st.pipe = pipe;
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    if ((stages & shader_stage::compute) == 0) return;
    std::lock_guard<std::mutex> lock(g_listMutex);
    ListState& st = g_listState[cl->get_native()];
    if (st.layout.handle != layout.handle) {
        st.layout = layout;
        std::memset(st.tables, 0, sizeof(st.tables));
    }
    for (uint32_t i = 0; i < count && first + i < 8; ++i) st.tables[first + i] = tables[i];
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& u) {
    if ((stages & shader_stage::compute) == 0 || param >= 8 || u.type != descriptor_type::constant_buffer || u.count < 1) return;
    std::lock_guard<std::mutex> lock(g_listMutex);
    ListState& st = g_listState[cl->get_native()];
    if (st.layout.handle != layout.handle) {
        st.layout = layout;
        std::memset(st.tables, 0, sizeof(st.tables));
        std::memset(st.cbv, 0, sizeof(st.cbv));
    }
    st.cbv[param] = static_cast<const buffer_range*>(u.descriptors)[0];
}

void OnBarrier(command_list*, uint32_t count, const resource* res, const resource_usage*, const resource_usage* to) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    if (g_tracked.empty()) return;
    for (uint32_t i = 0; i < count; ++i)
        if (g_tracked.count(res[i].handle)) g_state[res[i].handle] = to[i];
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_tracked.erase(r.handle);
    g_state.erase(r.handle);
    for (auto* views : {&g_uavs, &g_srvs}) {
        auto it = views->find(r.handle);
        if (it != views->end()) {
            if (device* dev = CurrentDevice()) dev->destroy_resource_view(it->second);
            views->erase(it);
        }
    }
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    ListState st;
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        auto it = g_listState.find(cl->get_native());
        if (it == g_listState.end()) return false;
        st = it->second;
    }
    const int pass = PassFromCs(st.cs);
    if (pass < 0) return false;
    device* dev = cl->get_device();

    BindingMap m;
    m.slots = Resolve(dev, st);
    m.frame = PresentCount();
    m.gx = x, m.gy = y, m.gz = z;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        BindingMap& old = g_maps[pass];
        m.dispatches = old.frame == m.frame ? old.dispatches + 1 : 1;
        for (const Slot& s : m.slots) g_tracked.insert(s.res.handle);
        first = !g_seenPass[pass];
        g_seenPass[pass] = true;
        old = m;
    }
    const bool dump = g_dumpPending[pass].exchange(false);
    if (first || dump) LogMap(pass, m);
    if (dump) DumpConstants(dev, pass, st);

    if (pass == kComposite && g_rrWanted.load()) RunGuides(cl, st, m);
    if (pass == kGiTemporal && ResetCountOn() && RunCountReset(cl, st, m)) ++g_actions;
    if (pass == kGiTemporal && ClearHistoryOn()) {
        // History = inputs of the temporal pass that the final blur wrote last frame.
        BindingMap blur;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            blur = g_maps[kGiBlur];
        }
        for (const Slot& s : m.slots) {
            if (s.reg[0] != 't') continue;
            bool isHistory = false;
            for (const Slot& b : blur.slots)
                if (b.reg[0] == 'u' && b.res.handle == s.res.handle) isHistory = true;
            if (!isHistory) continue;
            resource_usage state;
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                state = StateOf(s.res);
            }
            if (ClearTex(cl, s.res, state)) ++g_actions;
        }
    }
    if (pass == kGiBlur && BlurCopyOn()) {
        if (RunCentre(cl, st, m)) {
            ++g_actions;
            return true;  // the centre-only pass replaces the blur
        }
    }
    if (kPasses[pass].skippable && SkipOn(pass)) {
        ++g_actions;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- texture viewer

std::atomic<int> g_viewPass{-1};
char g_viewReg[8] = {};
std::mutex g_viewMutex;  // g_viewReg, view texture
resource g_viewTex = {0};
resource_view g_viewSrv = {0};
resource_desc g_viewDesc;
std::string g_viewInfo;

void DestroyView(device* dev) {
    if (g_viewSrv.handle) dev->destroy_resource_view(g_viewSrv);
    if (g_viewTex.handle) dev->destroy_resource(g_viewTex);
    g_viewSrv = {0};
    g_viewTex = {0};
}

bool Viewable(format f) {
    switch (f) {
        case format::r32g32b32a32_float: case format::r16g16b16a16_float: case format::r11g11b10_float:
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r10g10b10a2_unorm:
        case format::r16g16_float: case format::r32_float: case format::r16_float: case format::r8_unorm:
        case format::r16g16_snorm: case format::r16g16_unorm: case format::r8g8_unorm:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------- brightness measurement
//
// One click: every colour texture bound to the viewer's pass is copied to a readback buffer, the
// queue is waited on (a one-off hitch), and the average R, G, B is computed on the CPU. Lets the
// "denoiser off" result be compared with the game's own for a brightness shift.

std::string g_measure;  // g_viewMutex

float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = m / 1024.0f / 16384.0f;
    else if (e == 31) v = 0;  // inf/nan: ignore
    else v = (1.0f + m / 1024.0f) * ldexpf(1.0f, static_cast<int>(e) - 15);
    return s ? -v : v;
}

float SmallFloat(uint32_t bits, int mantBits) {  // unsigned 5-bit-exponent floats of r11g11b10
    const uint32_t e = bits >> mantBits, m = bits & ((1u << mantBits) - 1);
    if (e == 31) return 0;
    if (e == 0) return m / static_cast<float>(1u << mantBits) / 16384.0f;
    return (1.0f + m / static_cast<float>(1u << mantBits)) * ldexpf(1.0f, static_cast<int>(e) - 15);
}

bool Measurable(format f, uint32_t* bpp) {
    switch (f) {
        case format::r16g16b16a16_float: *bpp = 8; return true;
        case format::r32g32b32a32_float: *bpp = 16; return true;
        case format::r11g11b10_float: *bpp = 4; return true;
        default: return false;
    }
}

std::string MeasureOne(command_queue* q, resource src, resource_usage state) {
    device* dev = q->get_device();
    const resource_desc d = dev->get_resource_desc(src);
    uint32_t bpp = 0;
    if (d.type != resource_type::texture_2d || !Measurable(d.texture.format, &bpp)) return "";
    if (state == resource_usage::undefined) return "state unknown";
    const uint32_t w = d.texture.width, h = d.texture.height;
    const uint32_t pitch = (w * bpp + 255) & ~255u;
    resource buf = {0};
    if (!dev->create_resource(resource_desc(static_cast<uint64_t>(pitch) * h, memory_heap::readback, resource_usage::copy_dest), nullptr,
                              resource_usage::copy_dest, &buf))
        return "readback buffer failed";
    command_list* cl = q->get_immediate_command_list();
    cl->barrier(src, state, resource_usage::copy_source);
    cl->copy_texture_to_buffer(src, 0, nullptr, buf, 0, pitch / bpp, h);
    cl->barrier(src, resource_usage::copy_source, state);
    q->flush_immediate_command_list();
    q->wait_idle();
    std::string out = "map failed";
    void* data = nullptr;
    if (dev->map_buffer_region(buf, 0, static_cast<uint64_t>(pitch) * h, map_access::read_only, &data) && data) {
        double sum[3] = {}, mx = 0;
        uint64_t n = 0;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * pitch;
            for (uint32_t x = 0; x < w; ++x) {
                float c[3];
                if (bpp == 8) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
                    for (int i = 0; i < 3; ++i) c[i] = HalfToFloat(p[i]);
                } else if (bpp == 16) {
                    const float* p = reinterpret_cast<const float*>(row) + x * 4;
                    for (int i = 0; i < 3; ++i) c[i] = std::isfinite(p[i]) ? p[i] : 0;
                } else {
                    const uint32_t v = reinterpret_cast<const uint32_t*>(row)[x];
                    c[0] = SmallFloat(v & 0x7ff, 6), c[1] = SmallFloat((v >> 11) & 0x7ff, 6), c[2] = SmallFloat(v >> 22, 5);
                }
                for (int i = 0; i < 3; ++i) sum[i] += c[i];
                const double l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
                if (l > mx) mx = l;
                ++n;
            }
        }
        dev->unmap_buffer_region(buf);
        out = Format("avg %.4f %.4f %.4f  lum %.4f  max lum %.2f", sum[0] / n, sum[1] / n, sum[2] / n,
                     (0.2126 * sum[0] + 0.7152 * sum[1] + 0.0722 * sum[2]) / n, mx);
    }
    dev->destroy_resource(buf);
    return out;
}

// Every measurable texture of one pass, logged with a label.
std::string MeasurePass(command_queue* q, int mp, const char* label) {
    BindingMap m;
    std::vector<resource_usage> states;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        m = g_maps[mp];
        for (const Slot& s : m.slots) states.push_back(StateOf(s.res));
    }
    std::string all = Format("%s, %s:", kPasses[mp].name, label);
    for (size_t i = 0; i < m.slots.size(); ++i) {
        const std::string r = MeasureOne(q, m.slots[i].res, states[i]);
        if (!r.empty()) all += Format("\n  %s %s", m.slots[i].reg, r.c_str());
    }
    Log("measure %s", all.c_str());
    return all;
}

// Sweep: each experiment combination in turn, 90 presents to settle, then rt-composite and
// gi-temporal are measured. The user's own switches are put back afterwards.
struct SweepConfig {
    const char* name;
    int cap;  // frame count cap, -1 = not capped
    bool centre, skipMipsGap;
};
const SweepConfig kSweep[] = {
    {"all off (game)", -1, false, false},
    {"spatial off (centre + skips)", -1, true, true},
    {"spatial off, count cap 7", 7, true, true},
    {"spatial off, count cap 3", 3, true, true},
    {"spatial off, count cap 1", 1, true, true},
    {"spatial off, count cap 0", 0, true, true},
    {"count cap 0 only (spatial on)", 0, false, false},
};
constexpr int kSweepCount = sizeof(kSweep) / sizeof(kSweep[0]);
constexpr int kSweepSettle = 90;
std::atomic<bool> g_sweepRequested{false};
int g_sweepIndex = -1, g_sweepWait = 0;
bool g_savedFlags[5] = {};  // denoiserOff, resetCount, blurCopy, skip mips, skip gapfix
int g_savedCap = 0;
std::string g_sweepResult;  // g_viewMutex

void ApplySweep(const SweepConfig& c) {
    g_denoiserOff = false;
    g_clearHistory = false;
    g_resetCount = c.cap >= 0;
    if (c.cap >= 0) g_countCap = c.cap;
    g_blurCopy = c.centre;
    g_skip[kGiMips] = c.skipMipsGap;
    g_skip[kGiGapFix] = c.skipMipsGap;
}

void SweepStep(command_queue* q) {
    if (g_sweepRequested.exchange(false) && g_sweepIndex < 0) {
        g_savedFlags[0] = g_denoiserOff, g_savedFlags[1] = g_resetCount, g_savedFlags[2] = g_blurCopy;
        g_savedFlags[3] = g_skip[kGiMips], g_savedFlags[4] = g_skip[kGiGapFix];
        g_savedCap = g_countCap;
        g_sweepIndex = 0;
        g_sweepWait = kSweepSettle;
        ApplySweep(kSweep[0]);
        std::lock_guard<std::mutex> vlock(g_viewMutex);
        g_sweepResult = "sweep running...";
        Log("sweep started");
        return;
    }
    if (g_sweepIndex < 0 || --g_sweepWait > 0) return;
    const SweepConfig& c = kSweep[g_sweepIndex];
    const std::string comp = MeasurePass(q, kComposite, c.name);
    MeasurePass(q, kGiTemporal, c.name);
    // Short line for the overlay: lit colour (u0) and GI B (t10) of the composite.
    auto pick = [&](const char* reg) {
        const size_t p = comp.find(Format("\n  %s avg", reg));
        if (p == std::string::npos) return std::string("-");
        const size_t l = comp.find("lum ", p);
        return l == std::string::npos ? std::string("-") : comp.substr(l + 4, comp.find(' ', l + 4) - (l + 4));
    };
    {
        std::lock_guard<std::mutex> vlock(g_viewMutex);
        if (g_sweepIndex == 0) g_sweepResult.clear();
        g_sweepResult += Format("%-32s lit lum %s, GI B lum %s\n", c.name, pick("u0").c_str(), pick("t10").c_str());
    }
    if (++g_sweepIndex < kSweepCount) {
        ApplySweep(kSweep[g_sweepIndex]);
        g_sweepWait = kSweepSettle;
        return;
    }
    g_sweepIndex = -1;
    g_denoiserOff = g_savedFlags[0], g_resetCount = g_savedFlags[1], g_blurCopy = g_savedFlags[2];
    g_skip[kGiMips] = g_savedFlags[3], g_skip[kGiGapFix] = g_savedFlags[4];
    g_countCap = g_savedCap;
    Log("sweep done");
}

void OnPresent(command_queue* q, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    if (g_dumpRequested.exchange(false))
        for (int i = 0; i < kPassCount; ++i) g_dumpPending[i] = true;

    if (g_measureRequested.exchange(false) && g_viewPass.load() >= 0) {
        const std::string all = MeasurePass(q, g_viewPass.load(), g_denoiserOff.load() ? "denoiser OFF" : "denoiser on");
        std::lock_guard<std::mutex> vlock(g_viewMutex);
        g_measure = all;
    }
    SweepStep(q);

    const int pass = g_viewPass.load();
    device* dev = q->get_device();
    std::lock_guard<std::mutex> vlock(g_viewMutex);
    if (pass < 0) {
        g_viewInfo.clear();
        return;
    }
    resource src = {0};
    resource_usage state = resource_usage::undefined;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        src = Find(g_maps[pass], g_viewReg);
        if (src.handle) state = StateOf(src);
    }
    if (!src.handle) {
        g_viewInfo = "not bound this frame";
        return;
    }
    const resource_desc d = dev->get_resource_desc(src);
    if (d.type != resource_type::texture_2d || !Viewable(d.texture.format)) {
        g_viewInfo = Desc(src) + ": format not viewable";
        return;
    }
    if (state == resource_usage::undefined) {
        g_viewInfo = Desc(src) + ": state unknown yet";
        return;
    }
    if (!g_viewTex.handle || g_viewDesc.texture.width != d.texture.width || g_viewDesc.texture.height != d.texture.height ||
        g_viewDesc.texture.format != d.texture.format) {
        DestroyView(dev);
        resource_desc nd(d.texture.width, d.texture.height, 1, 1, d.texture.format, 1, memory_heap::default_,
                         resource_usage::copy_dest | resource_usage::shader_resource);
        if (!dev->create_resource(nd, nullptr, resource_usage::shader_resource, &g_viewTex) ||
            !dev->create_resource_view(g_viewTex, resource_usage::shader_resource,
                                       resource_view_desc(d.texture.format), &g_viewSrv)) {
            DestroyView(dev);
            g_viewInfo = "couldn't create the view texture";
            return;
        }
        g_viewDesc = nd;
    }
    command_list* cl = q->get_immediate_command_list();
    if (CopyTex(cl, src, state, g_viewTex, resource_usage::shader_resource)) {
        q->flush_immediate_command_list();
        g_viewInfo = Desc(src);
    } else {
        g_viewInfo = Desc(src) + ": copy refused";
    }
}

}  // namespace

// ---------------------------------------------------------------- public

void Register() {
    descriptor_tracking::register_events();
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnReset);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
}

void Unregister() { descriptor_tracking::unregister_events(); }

void OnDestroyDevice(device* dev) {
    std::lock_guard<std::mutex> vlock(g_viewMutex);
    DestroyView(dev);
    std::lock_guard<std::mutex> lock(g_mapMutex);
    for (auto& kv : g_uavs) dev->destroy_resource_view(kv.second);
    for (auto& kv : g_srvs) dev->destroy_resource_view(kv.second);
    g_uavs.clear();
    g_srvs.clear();
    for (int i = 0; i < 3; ++i) {
        if (g_guideUav[i].handle) dev->destroy_resource_view(g_guideUav[i]);
        if (g_guideTex[i].handle) dev->destroy_resource(g_guideTex[i]);
        g_guideUav[i] = {0};
        g_guideTex[i] = {0};
    }
    for (resource r : g_guideRetired) dev->destroy_resource(r);
    g_guideRetired.clear();
    g_guideW = g_guideH = 0;
    if (g_guidePipe.handle) dev->destroy_pipeline(g_guidePipe);
    if (g_guideLayout.handle) dev->destroy_pipeline_layout(g_guideLayout);
    g_guidePipe = {0};
    g_guideLayout = {0};
    g_guideFailed = false;
    {
        std::lock_guard<std::mutex> glock(g_guideMutex);
        g_guidesValid = false;
    }
    if (g_resetPipe.handle) dev->destroy_pipeline(g_resetPipe);
    if (g_resetLayout.handle) dev->destroy_pipeline_layout(g_resetLayout);
    g_resetPipe = {0};
    g_resetLayout = {0};
    g_resetFailed = false;
    if (g_centrePipe.handle) dev->destroy_pipeline(g_centrePipe);
    if (g_centreLayout.handle) dev->destroy_pipeline_layout(g_centreLayout);
    g_centrePipe = {0};
    g_centreLayout = {0};
    g_centreFailed = false;
}

void RequestDump() { g_dumpRequested = true; }

bool LatestGuides(Guides* out) {
    std::lock_guard<std::mutex> lock(g_guideMutex);
    if (!g_guidesValid) return false;
    *out = g_guides;
    return true;
}

bool RrWanted() { return g_rrWanted.load(); }

int RrPreset() { return g_rrPreset.load(); }

void SetRrWorking(bool on) { g_rrWorking = on; }

// ---------------------------------------------------------------- settings (ReShade.ini [MetroExodusRR])

const char* const kRrModelNames[] = {"NVIDIA default", "D", "E", "F (latest)"};
const int kRrModelValues[] = {0, 4, 5, 6};

void LoadSettings() {
    bool rrOn = true;
    if (reshade::get_config_value(nullptr, MD_PRODUCT, "RayReconstruction", rrOn)) g_rrWanted = rrOn;
    int preset = 0;
    if (reshade::get_config_value(nullptr, MD_PRODUCT, "RRModel", preset) && (preset == 0 || (preset >= 4 && preset <= 6)))
        g_rrPreset = preset;
    int frames = 3;
    if (reshade::get_config_value(nullptr, MD_PRODUCT, "GiHistoryFrames", frames) && frames >= 0 && frames <= 31) g_countCap = frames;
    Log("settings: Ray Reconstruction %s, RR model %d, GI history frames %d", g_rrWanted.load() ? "on" : "off", g_rrPreset.load(),
        g_countCap.load());
}

void Tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void DrawSettings() {
    bool rrOn = g_rrWanted.load();
    if (ImGui::Checkbox("Ray Reconstruction", &rrOn)) {
        g_rrWanted = rrOn;
        reshade::set_config_value(nullptr, MD_PRODUCT, "RayReconstruction", rrOn);
    }
    Tip("DLSS Ray Reconstruction in place of the game's DLSS: one AI model denoises the ray-traced global\n"
        "illumination and reflections and upscales. The game's GI denoiser is switched off while it runs.\n"
        "Off: the game's own DLSS and denoiser, unchanged. Needs DLSS selected in the game's options.");
    int idx = 0;
    for (int i = 0; i < 4; ++i)
        if (kRrModelValues[i] == g_rrPreset.load()) idx = i;
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Ray Reconstruction model", &idx, kRrModelNames, 4)) {
        g_rrPreset = kRrModelValues[idx];
        reshade::set_config_value(nullptr, MD_PRODUCT, "RRModel", g_rrPreset.load());
    }
    Tip("NVIDIA default lets the driver pick. D, E and F are the transformer models; F is the latest.");
    int cap = g_countCap.load();
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("GI history frames", &cap, 0, 8)) {
        g_countCap = cap;
        reshade::set_config_value(nullptr, MD_PRODUCT, "GiHistoryFrames", cap);
    }
    Tip("How many frames the game's own GI accumulation keeps before Ray Reconstruction (default 3).\n"
        "Fewer: rawer GI for Ray Reconstruction, but the game's GI encoding loses brightness\n"
        "(0 = about 12 % darker). More: less noise in, more lag in moving light.");
    ImGui::TextDisabled("Status: %s", rr::Status().c_str());
    ImGui::TextDisabled("Game's GI denoiser: %s", DenoiserOff() ? "off (Ray Reconstruction denoises)" : "on");
}

void DrawAdvanced() {
    ImGui::TextDisabled("Guides made: %llu%s%s", g_guideRuns.load(), g_guideError.empty() ? "" : ", ", g_guideError.c_str());
    ImGui::TextDisabled("Passes seen:");
    for (int i = 0; i < kPassCount; ++i) {
        BindingMap m;
        bool seen;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            m = g_maps[i];
            seen = g_seenPass[i];
        }
        const uint64_t age = PresentCount() - m.frame;
        ImGui::BulletText("%s (cs%08x): %s", kPasses[i].name, kPasses[i].cs,
                          !seen ? "never" : age <= 2 ? Format("active, %u dispatch(es)", m.dispatches).c_str() : "not this frame");
    }
    ImGui::Separator();
    bool v = g_denoiserOff.load();
    if (ImGui::Checkbox("GI denoiser OFF (all of the below at once)", &v)) g_denoiserOff = v;
    ImGui::TextDisabled("Individual experiments:");
    v = g_resetCount.load();
    if (ImGui::Checkbox("GI: cap the frame count", &v)) g_resetCount = v;
    ImGui::SameLine();
    int cap = g_countCap.load();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("max frames of history", &cap, 0, 31)) g_countCap = cap;  // not saved: testing
    if (!g_resetError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", g_resetError.c_str());
    ImGui::TextDisabled("frame count resets run: %llu", g_resetRuns.load());
    v = g_clearHistory.load();
    if (ImGui::Checkbox("GI: clear history (darkens: reference only)", &v)) g_clearHistory = v;
    v = g_blurCopy.load();
    if (ImGui::Checkbox("GI: replace the final spatial blur with the centre sample only", &v)) g_blurCopy = v;
    if (!g_centreError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", g_centreError.c_str());
    ImGui::TextDisabled("centre-only passes run: %llu", g_centreRuns.load());
    for (int i = 0; i < kPassCount; ++i) {
        if (!kPasses[i].skippable) continue;
        v = g_skip[i].load();
        if (ImGui::Checkbox(Format("Skip %s", kPasses[i].name).c_str(), &v)) g_skip[i] = v;
    }
    ImGui::TextDisabled("experiment actions so far: %llu", g_actions.load());
    if (ImGui::Button("Dump bindings + constants (F11)")) RequestDump();
    ImGui::SameLine();
    if (ImGui::Button("Run sweep (every combination, ~10 s, stand still)")) g_sweepRequested = true;
    {
        std::lock_guard<std::mutex> vlock(g_viewMutex);
        if (!g_sweepResult.empty()) ImGui::TextUnformatted(g_sweepResult.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Texture viewer (copied at present):");
    int pass = g_viewPass.load();
    if (ImGui::BeginCombo("pass", pass < 0 ? "off" : kPasses[pass].name)) {
        if (ImGui::Selectable("off", pass < 0)) g_viewPass = -1;
        for (int i = 0; i < kPassCount; ++i)
            if (ImGui::Selectable(kPasses[i].name, pass == i)) g_viewPass = i;
        ImGui::EndCombo();
    }
    pass = g_viewPass.load();
    if (pass < 0) return;
    BindingMap m;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        m = g_maps[pass];
    }
    std::lock_guard<std::mutex> vlock(g_viewMutex);
    for (const Slot& s : m.slots) {
        if (ImGui::RadioButton(s.reg, strcmp(g_viewReg, s.reg) == 0)) strcpy(g_viewReg, s.reg);
        ImGui::SameLine();
        ImGui::TextUnformatted(Desc(s.res).c_str());
    }
    ImGui::TextUnformatted(g_viewInfo.c_str());
    if (ImGui::Button("Measure brightness of this pass's textures")) g_measureRequested = true;
    if (!g_measure.empty()) ImGui::TextUnformatted(g_measure.c_str());
    if (g_viewSrv.handle && g_viewDesc.texture.width) {
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = w * g_viewDesc.texture.height / static_cast<float>(g_viewDesc.texture.width);
        ImGui::Image(g_viewSrv.handle, ImVec2(w, h));
    }
}

}  // namespace rt
