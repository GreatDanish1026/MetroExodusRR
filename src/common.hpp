// Shared between main.cpp (log, frame capture, ReShade events) and ngx.cpp (the game's DLSS calls).
#pragma once

#include <cstdint>
#include <string>

namespace reshade::api { struct device; }

void Log(const char* fmt, ...);
std::string Format(const char* fmt, ...);

// Short description of a D3D12 resource ("#1a2b3 tex 2560x1440 rgba16f"), same ids as the capture.
std::string DescribeNative(void* d3d12Resource);

// Adds a line to the frame capture under the native command list it was recorded on (no-op when
// not capturing).
void CaptureNote(uint64_t nativeCommandList, const std::string& line);

uint64_t PresentCount();

// Stable id (FNV hash of the bytecode) of a pipeline's compute shader, 0 if none/unknown.
uint32_t ComputeShaderId(uint64_t pipelineHandle);
// Folder of the game exe, with a trailing backslash.
const std::wstring& ExeDir();
// The game's D3D12 device (nullptr before it exists).
reshade::api::device* CurrentDevice();

namespace rt {
void Register();
void Unregister();
void OnDestroyDevice(reshade::api::device* dev);
void RequestDump();  // F11: binding maps + constant buffers of the known passes
void DrawOverlay();
}  // namespace rt

namespace ngx {
void Install();    // hooks GetProcAddress in MetroExodus.exe's import table
void Uninstall();  // restores it; the game's NGX pointers fall back to the driver's functions
void OnPresent();  // periodic summary
std::string Status();
}  // namespace ngx
