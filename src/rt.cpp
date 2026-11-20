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
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
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
std::unordered_map<uint64_t, resource_view> g_uavs;    // our UAVs for clearing history

// Experiments (overlay, live)
std::atomic<bool> g_skip[kPassCount] = {};
std::atomic<bool> g_blurCopy{false};
std::atomic<bool> g_blurCopySwap{false};
std::atomic<bool> g_clearHistory{false};
std::atomic<bool> g_dumpRequested{false};
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
    g_listState[cl->get_native()].cs = cs;
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
    auto it = g_uavs.find(r.handle);
    if (it != g_uavs.end()) {
        if (device* dev = CurrentDevice()) dev->destroy_resource_view(it->second);
        g_uavs.erase(it);
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

    if (pass == kGiTemporal && g_clearHistory.load()) {
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
    if (pass == kGiBlur && g_blurCopy.load()) {
        const bool swap = g_blurCopySwap.load();
        const resource in0 = Find(m, swap ? "t4" : "t3"), in1 = Find(m, swap ? "t3" : "t4");
        const resource out0 = Find(m, "u0"), out1 = Find(m, "u1");
        if (in0.handle && in1.handle && out0.handle && out1.handle) {
            resource_usage s[4];
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                s[0] = StateOf(in0), s[1] = StateOf(in1), s[2] = StateOf(out0), s[3] = StateOf(out1);
            }
            const bool ok = CopyTex(cl, in0, s[0], out0, s[2]) && CopyTex(cl, in1, s[1], out1, s[3]);
            if (ok) {
                ++g_actions;
                return true;  // the copies replace the blur
            }
        }
    }
    if (kPasses[pass].skippable && g_skip[pass].load()) {
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

void OnPresent(command_queue* q, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    if (g_dumpRequested.exchange(false))
        for (int i = 0; i < kPassCount; ++i) g_dumpPending[i] = true;

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
    g_uavs.clear();
}

void RequestDump() { g_dumpRequested = true; }

void DrawOverlay() {
    if (!ImGui::CollapsingHeader("Ray tracing passes (experiments)", ImGuiTreeNodeFlags_DefaultOpen)) return;
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
    bool v = g_clearHistory.load();
    if (ImGui::Checkbox("GI: clear history before the temporal pass", &v)) g_clearHistory = v;
    v = g_blurCopy.load();
    if (ImGui::Checkbox("GI: replace the final spatial blur with a copy", &v)) g_blurCopy = v;
    ImGui::SameLine();
    v = g_blurCopySwap.load();
    if (ImGui::Checkbox("swap inputs", &v)) g_blurCopySwap = v;
    for (int i = 0; i < kPassCount; ++i) {
        if (!kPasses[i].skippable) continue;
        v = g_skip[i].load();
        if (ImGui::Checkbox(Format("Skip %s", kPasses[i].name).c_str(), &v)) g_skip[i] = v;
    }
    ImGui::TextDisabled("experiment actions so far: %llu", g_actions.load());
    if (ImGui::Button("Dump bindings + constants (F11)")) RequestDump();

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
    if (g_viewSrv.handle && g_viewDesc.texture.width) {
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = w * g_viewDesc.texture.height / static_cast<float>(g_viewDesc.texture.width);
        ImGui::Image(g_viewSrv.handle, ImVec2(w, h));
    }
}

}  // namespace rt
