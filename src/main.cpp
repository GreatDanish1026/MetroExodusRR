// MetroExodusRR: a ReShade add-on for Metro Exodus Enhanced Edition (4A Engine, DirectX 12) that
// will replace the game's ray-tracing denoisers and DLSS Super Resolution with DLSS Ray
// Reconstruction.
//
// 0.0.2 adds live experiments on the game's ray-tracing denoiser (rt.cpp); with every experiment
// off it still changes nothing. The probe parts:
// - ngx.cpp logs every call the game makes to DLSS (NGX) with all of its inputs.
// - Every compute shader the game creates is saved once to MetroExodusRR-shaders\csXXXXXXXX.dxil
//   (same id as in the capture) with its debug name in the log, so the denoiser passes can be
//   found and read offline.
// - F10 records two frames of GPU work to MetroExodusRR-frame-N.txt next to the game exe: draws,
//   dispatches and ray dispatches with stable ids of their shaders, resource transitions, bound
//   views and constant buffers, copies, render targets, and where the game's DLSS call sits
//   ("NGX EVALUATE"). Ray Reconstruction needs the lit picture before denoising plus G-buffer
//   guides (albedo, normals, roughness), so the capture has to show where they are made.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = MD_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION = "DLSS Ray Reconstruction for " MD_GAME " (in development).";

using namespace reshade::api;

namespace {

constexpr int kCaptureHotkey = VK_F10;
constexpr int kDumpHotkey = VK_F11;
constexpr int kCaptureFrames = 2;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;
std::atomic<device*> g_device{nullptr};
std::atomic<uint64_t> g_presents{0};

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    // ReShade loads add-ons again when the game makes another device: the same process appends.
    wchar_t pid[32] = {}, mine[32] = {};
    swprintf(mine, 32, L"%lu", GetCurrentProcessId());
    const bool again = GetEnvironmentVariableW(L"METROEXRR_LOG_PID", pid, 32) && wcscmp(pid, mine) == 0;
    SetEnvironmentVariableW(L"METROEXRR_LOG_PID", mine);
    g_log = _wfopen((g_exeDir + L"" MD_PRODUCT_W L".log").c_str(), again ? L"a" : L"w");
    Log(MD_PRODUCT " " MD_VERSION " loaded%s", again ? " again (same game session)" : "");
}

// ---------------------------------------------------------------- shader ids
//
// Pipeline handles change every run; a hash of the shader bytecode doesn't, so captures from
// different runs can be compared.

struct PipeIds {
    uint32_t cs = 0, vs = 0, ps = 0;
};
std::mutex g_pipeMutex;
std::unordered_map<uint64_t, PipeIds> g_pipes;

uint32_t Fnv(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Debug name from the DXIL/DXBC container's ILDN part ("" if the shader has none).
std::string DebugName(const void* code, size_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(code);
    if (size < 32 || std::memcmp(b, "DXBC", 4) != 0) return {};
    uint32_t parts = 0;
    std::memcpy(&parts, b + 28, 4);
    for (uint32_t i = 0; i < parts && 32 + 4 * (i + 1) <= size; ++i) {
        uint32_t off = 0;
        std::memcpy(&off, b + 32 + 4 * i, 4);
        if (off + 8 > size || std::memcmp(b + off, "ILDN", 4) != 0) continue;
        uint32_t len = 0;
        std::memcpy(&len, b + off + 4, 4);
        if (off + 8 + len > size || len < 4) return {};
        uint16_t nameLen = 0;
        std::memcpy(&nameLen, b + off + 10, 2);
        if (4u + nameLen > len) return {};
        return std::string(reinterpret_cast<const char*>(b + off + 12), nameLen);
    }
    return {};
}

std::mutex g_savedMutex;
std::unordered_map<uint32_t, bool> g_saved;

// Each compute shader once per run: the file for offline disassembly, its name in the log.
void SaveComputeShader(uint32_t id, const void* code, size_t size) {
    {
        std::lock_guard<std::mutex> lock(g_savedMutex);
        if (!g_saved.emplace(id, true).second) return;
    }
    const std::wstring dir = g_exeDir + MD_PRODUCT_W L"-shaders\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t file[32];
    swprintf(file, 32, L"cs%08x.dxil", id);
    const std::wstring path = dir + file;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            fwrite(code, 1, size, f);
            fclose(f);
        }
    }
    const std::string name = DebugName(code, size);
    Log("compute shader cs%08x %zu bytes%s%s", id, size, name.empty() ? "" : " name ", name.c_str());
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    PipeIds ids;
    for (uint32_t i = 0; i < count; ++i) {
        if (!subobjects[i].data) continue;
        const pipeline_subobject_type t = subobjects[i].type;
        if (t != pipeline_subobject_type::compute_shader && t != pipeline_subobject_type::vertex_shader &&
            t != pipeline_subobject_type::pixel_shader)
            continue;
        const shader_desc& s = *static_cast<const shader_desc*>(subobjects[i].data);
        if (!s.code || !s.code_size) continue;
        const uint32_t h = Fnv(s.code, s.code_size);
        if (t == pipeline_subobject_type::compute_shader) SaveComputeShader(h, s.code, s.code_size);
        if (t == pipeline_subobject_type::compute_shader) ids.cs = h;
        else if (t == pipeline_subobject_type::vertex_shader) ids.vs = h;
        else ids.ps = h;
    }
    if (!ids.cs && !ids.vs && !ids.ps) return;
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_pipes[pipe.handle] = ids;
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_pipes.erase(pipe.handle);
}

PipeIds Pipe(pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_pipes.find(pipe.handle);
    return it == g_pipes.end() ? PipeIds{} : it->second;
}

// ---------------------------------------------------------------- resource descriptions

const char* FormatName(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 2: return "rgba32f";
        case 10: return "rgba16f";
        case 11: return "rgba16unorm";
        case 16: return "rg32f";
        case 19: return "r32g8x24_typeless";
        case 20: return "d32s8";
        case 23: return "rgb10a2_typeless";
        case 24: return "rgb10a2unorm";
        case 26: return "r11g11b10f";
        case 27: return "rgba8_typeless";
        case 28: return "rgba8unorm";
        case 29: return "rgba8srgb";
        case 34: return "rg16f";
        case 35: return "rg16unorm";
        case 37: return "rg16snorm";
        case 39: return "r32_typeless";
        case 40: return "d32f";
        case 41: return "r32f";
        case 42: return "r32uint";
        case 44: return "r24g8_typeless";
        case 45: return "d24s8";
        case 49: return "rg8unorm";
        case 54: return "r16f";
        case 55: return "d16";
        case 56: return "r16unorm";
        case 57: return "r16uint";
        case 61: return "r8unorm";
        case 62: return "r8uint";
        case 87: return "bgra8unorm";
        case 91: return "bgra8srgb";
        default: return nullptr;
    }
}

std::string Describe(device* dev, resource r) {
    if (!r.handle) return "null";
    if (!dev) return Format("#%05llx", (r.handle >> 4) & 0xfffff);
    resource_desc d = dev->get_resource_desc(r);
    std::string id = Format("#%05llx", (r.handle >> 4) & 0xfffff);  // short, enough to follow one resource
    if (d.type == resource_type::buffer) return id + Format(" buf %llu", d.buffer.size);
    const char* fn = FormatName(d.texture.format);
    std::string fmt = fn ? fn : Format("fmt%u", static_cast<uint32_t>(d.texture.format));
    std::string s = id + Format(" tex %ux%u %s", d.texture.width, d.texture.height, fmt.c_str());
    if (d.texture.depth_or_layers > 1) s += Format(" x%u", d.texture.depth_or_layers);
    if (d.texture.levels > 1) s += Format(" mips%u", d.texture.levels);
    return s;
}

const char* UsageName(resource_usage u) {
    switch (u) {
        case resource_usage::undefined: return "undef";
        case resource_usage::general: return "general";
        case resource_usage::depth_stencil: return "ds";
        case resource_usage::indirect_argument: return "indirect";
        case resource_usage::constant_buffer: return "cbv";
        case resource_usage::render_target: return "rt";
        case resource_usage::depth_stencil_write: return "dsv";
        case resource_usage::depth_stencil_read: return "dsv_ro";
        case resource_usage::unordered_access: return "uav";
        case resource_usage::shader_resource_pixel: return "srv_ps";
        case resource_usage::shader_resource_non_pixel: return "srv_cs";
        case resource_usage::shader_resource: return "srv";
        case resource_usage::copy_dest: return "copy_dst";
        case resource_usage::copy_source: return "copy_src";
        case resource_usage::present: return "present";
        default: return nullptr;
    }
}

std::string Usage(resource_usage u) {
    const char* n = UsageName(u);
    return n ? n : Format("0x%x", static_cast<uint32_t>(u));
}

// ---------------------------------------------------------------- capture

enum class Capture { Idle, Recording, Writing };
std::atomic<Capture> g_capture{Capture::Idle};
int g_framesLeft = 0;
int g_captureCount = 0;
std::string g_status;

struct ListLog {
    std::vector<std::string> lines;
    bool partial = false;  // reset before the capture began: its first events are missing
    uint32_t draws = 0;    // consecutive draws not yet written
};

std::mutex g_capMutex;
std::unordered_map<uint64_t, ListLog> g_lists;  // native command list -> events since its reset
std::vector<std::string> g_frame;                // executed lists in order, with frame boundaries
std::unordered_map<uint64_t, uint64_t> g_listIds;
uint64_t g_nextListId = 1;

bool Recording() { return g_capture.load(std::memory_order_relaxed) == Capture::Recording; }

ListLog& List(uint64_t key) {  // g_capMutex held
    auto it = g_lists.find(key);
    if (it != g_lists.end()) return it->second;
    ListLog& l = g_lists[key];
    l.partial = true;
    return l;
}

void FlushDraws(ListLog& l) {
    if (!l.draws) return;
    l.lines.push_back(Format("  draw x%u", l.draws));
    l.draws = 0;
}

void AddKey(uint64_t key, std::string line) {
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(key);
    FlushDraws(l);
    l.lines.push_back("  " + std::move(line));
}

void Add(command_list* cl, std::string line) { AddKey(cl->get_native(), std::move(line)); }

void OnReset(command_list* cl) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    g_lists[cl->get_native()] = {};
}

void OnExecute(command_queue* q, command_list* cl) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    auto it = g_lists.find(cl->get_native());
    uint64_t& id = g_listIds[cl->get_native()];
    if (!id) id = g_nextListId++;
    const command_queue_type t = q->get_type();
    const char* qn = (t & command_queue_type::graphics) != 0  ? "graphics"
                     : (t & command_queue_type::compute) != 0 ? "compute"
                                                              : "copy";
    if (it == g_lists.end()) {
        g_frame.push_back(Format("EXECUTE list L%llu on %s queue %p (not recorded)", id, qn, q));
        return;
    }
    FlushDraws(it->second);
    g_frame.push_back(Format("EXECUTE list L%llu on %s queue %p, thread %lu, %zu events%s", id, qn, q, GetCurrentThreadId(),
                             it->second.lines.size(), it->second.partial ? " (started before the capture: partial)" : ""));
    for (std::string& s : it->second.lines) g_frame.push_back(std::move(s));
    g_lists.erase(it);
}

void OnBarrier(command_list* cl, uint32_t count, const resource* res, const resource_usage* from, const resource_usage* to) {
    if (!Recording()) return;
    device* dev = cl->get_device();
    for (uint32_t i = 0; i < count; ++i)
        Add(cl, Format("barrier %s: %s -> %s", Describe(dev, res[i]).c_str(), Usage(from[i]).c_str(), Usage(to[i]).c_str()));
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    if (!Recording()) return;
    const PipeIds ids = Pipe(pipe);
    if ((stages & pipeline_stage::compute_shader) != 0) Add(cl, Format("compute pipeline cs%08x", ids.cs));
    else Add(cl, Format("graphics pipeline vs%08x ps%08x", ids.vs, ids.ps));
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (Recording()) Add(cl, Format("DISPATCH %u x %u x %u", x, y, z));
    return false;
}

bool OnDispatchRays(command_list* cl, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t,
                    uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t w, uint32_t h, uint32_t d) {
    if (Recording()) Add(cl, Format("DISPATCH RAYS %u x %u x %u", w, h, d));
    return false;
}

bool OnDraw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    if (!Recording()) return false;
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl->get_native());
    // Small draws are usually full-screen passes or UI quads: write each one with its size.
    if (vertices <= 6 && instances == 1) {
        FlushDraws(l);
        l.lines.push_back(Format("  DRAW %u vertices", vertices));
    } else {
        ++l.draws;
    }
    return false;
}

bool OnDrawIndexed(command_list* cl, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    if (!Recording()) return false;
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl->get_native());
    if (indices <= 6 && instances == 1) {
        FlushDraws(l);
        l.lines.push_back(Format("  DRAW indexed %u", indices));
    } else {
        ++l.draws;
    }
    return false;
}

bool OnExecuteIndirect(command_list* cl, indirect_command type, resource, uint64_t, uint32_t count, uint32_t) {
    if (Recording()) Add(cl, Format("indirect type %u count %u", static_cast<uint32_t>(type), count));
    return false;
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout, uint32_t param, const descriptor_table_update& u) {
    if (!Recording()) return;
    device* dev = cl->get_device();
    const char* st = (stages & shader_stage::compute) != 0 ? "cs" : "gfx";
    std::string s = Format("push %s param %u at %u:", st, param, u.binding);
    for (uint32_t i = 0; i < u.count && i < 16; ++i) {
        switch (u.type) {
            case descriptor_type::constant_buffer: {
                const buffer_range& b = static_cast<const buffer_range*>(u.descriptors)[i];
                s += Format(" cbv(%s +%llu size %llu)", Describe(dev, b.buffer).c_str(), b.offset, b.size);
                break;
            }
            case descriptor_type::shader_resource_view:
            case descriptor_type::unordered_access_view: {
                resource_view v = static_cast<const resource_view*>(u.descriptors)[i];
                s += Format(" %s(%s)", u.type == descriptor_type::unordered_access_view ? "uav" : "srv",
                            v.handle ? Describe(dev, dev->get_resource_from_view(v)).c_str() : "null");
                break;
            }
            case descriptor_type::buffer_shader_resource_view:
            case descriptor_type::buffer_unordered_access_view: {
                const buffer_range& b = static_cast<const buffer_range*>(u.descriptors)[i];
                s += Format(" %s(%s +%llu)", u.type == descriptor_type::buffer_unordered_access_view ? "buav" : "bsrv",
                            Describe(dev, b.buffer).c_str(), b.offset);
                break;
            }
            default:
                s += Format(" type%u", static_cast<uint32_t>(u.type));
                break;
        }
    }
    Add(cl, s);
}

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first, uint32_t count,
                     const void* values) {
    if (!Recording()) return;
    std::string s = Format("constants %s param %u at %u:", (stages & shader_stage::compute) != 0 ? "cs" : "gfx", param, first);
    const float* f = static_cast<const float*>(values);
    for (uint32_t i = 0; i < count && i < 16; ++i) s += Format(" %g", f[i]);
    Add(cl, s);
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    if (!Recording()) return;
    const char* st = (stages & shader_stage::compute) != 0 ? "cs" : "gfx";
    std::string s = Format("tables %s %u..%u:", st, first, first + count - 1);
    for (uint32_t i = 0; i < count && i < 8; ++i) s += Format(" %llx", tables[i].handle);
    Add(cl, s);
}

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (!Recording()) return;
    device* dev = cl->get_device();
    std::string s = "render targets:";
    for (uint32_t i = 0; i < count; ++i)
        s += " " + (rtvs[i].handle ? Describe(dev, dev->get_resource_from_view(rtvs[i])) : std::string("null"));
    if (dsv.handle) s += " depth " + Describe(dev, dev->get_resource_from_view(dsv));
    Add(cl, s);
}

bool OnCopyResource(command_list* cl, resource src, resource dst) {
    if (Recording()) {
        device* dev = cl->get_device();
        Add(cl, Format("copy %s -> %s", Describe(dev, src).c_str(), Describe(dev, dst).c_str()));
    }
    return false;
}

bool OnCopyTexture(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t,
                   const subresource_box*, filter_mode) {
    if (Recording()) {
        device* dev = cl->get_device();
        Add(cl, Format("copy region %s -> %s", Describe(dev, src).c_str(), Describe(dev, dst).c_str()));
    }
    return false;
}

bool OnResolve(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t, uint32_t,
               uint32_t, format) {
    if (Recording()) {
        device* dev = cl->get_device();
        Add(cl, Format("resolve %s -> %s", Describe(dev, src).c_str(), Describe(dev, dst).c_str()));
    }
    return false;
}

void WriteCapture() {
    std::vector<std::string> frame;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        frame.swap(g_frame);
        g_lists.clear();
    }
    ++g_captureCount;
    std::wstring name = g_exeDir + L"" MD_PRODUCT_W L"-frame-" + std::to_wstring(g_captureCount) + L".txt";
    FILE* f = _wfopen(name.c_str(), L"w");
    if (!f) {
        g_status = "couldn't write the capture file";
        return;
    }
    size_t evaluates = 0;
    for (const std::string& s : frame) {
        fputs(s.c_str(), f);
        fputc('\n', f);
        if (s.find("NGX EVALUATE") != std::string::npos) ++evaluates;
    }
    fclose(f);
    g_status = Format("Capture %d: %zu lines, %zu DLSS calls -> " MD_PRODUCT "-frame-%d.txt", g_captureCount, frame.size(), evaluates,
                      g_captureCount);
    Log("%s", g_status.c_str());
}

void StartCapture() {
    if (g_capture.load() != Capture::Idle) return;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        g_lists.clear();
        g_frame.clear();
        g_frame.push_back("==== capture start (lists reset before this point are marked partial)");
    }
    g_framesLeft = kCaptureFrames;
    g_capture = Capture::Recording;
    Log("capture started");
}

// ---------------------------------------------------------------- device, swapchain, present

void OnInitDevice(device* dev) {
    Log("device created: api 0x%x", static_cast<uint32_t>(dev->get_api()));
    if (dev->get_api() == device_api::d3d12) g_device = dev;
}

void OnDestroyDevice(device* dev) {
    rt::OnDestroyDevice(dev);
    if (g_device == dev) g_device = nullptr;
}

void OnInitSwapchain(swapchain* sc, bool resize) {
    device* dev = sc->get_device();
    resource back = sc->get_current_back_buffer();
    Log("swapchain %s: %u back buffers, %s, colour space %u, api 0x%x, window %p", resize ? "resized" : "created",
        sc->get_back_buffer_count(), Describe(dev, back).c_str(), static_cast<uint32_t>(sc->get_color_space()),
        static_cast<uint32_t>(dev->get_api()), sc->get_hwnd());
}

bool g_keyWasDown = false, g_dumpKeyWasDown = false;

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    const uint64_t n = ++g_presents;
    if (n == 1) Log("first present");
    ngx::OnPresent();
    if (g_capture.load() == Capture::Recording) {
        resource back = sc->get_current_back_buffer();
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_frame.push_back(Format("==== PRESENT (back buffer %s, thread %lu), %d more",
                                     Describe(sc->get_device(), back).c_str(), GetCurrentThreadId(), g_framesLeft - 1));
        }
        if (--g_framesLeft <= 0) {
            g_capture = Capture::Writing;
            WriteCapture();
            g_capture = Capture::Idle;
        }
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kCaptureHotkey) & 0x8000);
    if (down && !g_keyWasDown) StartCapture();
    g_keyWasDown = down;
    const bool dumpDown = pid == GetCurrentProcessId() && (GetAsyncKeyState(kDumpHotkey) & 0x8000);
    if (dumpDown && !g_dumpKeyWasDown) rt::RequestDump();
    g_dumpKeyWasDown = dumpDown;
}

// ---------------------------------------------------------------- overlay

void DrawOverlay(effect_runtime*) {
    ImGui::TextDisabled(MD_PRODUCT " " MD_VERSION " (probe + experiments; all off = no change to the game)");
    ImGui::TextUnformatted(ngx::Status().c_str());
    ImGui::BeginDisabled(g_capture.load() != Capture::Idle);
    if (ImGui::Button("Capture frame (F10)")) StartCapture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_capture.load() == Capture::Recording ? "recording..." : g_status.c_str());
    rt::DrawOverlay();
}

}  // namespace

// ---------------------------------------------------------------- shared helpers (common.hpp)

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%6.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string DescribeNative(void* res) { return Describe(g_device.load(), resource{reinterpret_cast<uint64_t>(res)}); }

void CaptureNote(uint64_t nativeCommandList, const std::string& line) {
    if (Recording()) AddKey(nativeCommandList, line);
}

uint64_t PresentCount() { return g_presents.load(); }

uint32_t ComputeShaderId(uint64_t pipelineHandle) { return Pipe(pipeline{pipelineHandle}).cs; }

const std::wstring& ExeDir() { return g_exeDir; }

device* CurrentDevice() { return g_device.load(); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            OpenLog();
            ngx::Install();
            rt::Register();
            reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
            reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
            reshade::register_event<reshade::addon_event::init_swapchain>(&OnInitSwapchain);
            reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
            reshade::register_event<reshade::addon_event::reset_command_list>(&OnReset);
            reshade::register_event<reshade::addon_event::execute_command_list>(&OnExecute);
            reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
            reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnExecuteIndirect);
            reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
            reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
            reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
            reshade::register_event<reshade::addon_event::copy_texture_region>(&OnCopyTexture);
            reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(MD_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            ngx::Uninstall();
            rt::Unregister();
            reshade::unregister_addon(module);
            {
                std::lock_guard<std::mutex> lock(g_logMutex);
                if (g_log) fclose(g_log);
                g_log = nullptr;
            }
            break;
    }
    return TRUE;
}
