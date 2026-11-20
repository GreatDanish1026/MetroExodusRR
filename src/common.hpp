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

namespace ngx {
void Install();    // hooks GetProcAddress in MetroExodus.exe's import table
void Uninstall();  // restores it; the game's NGX pointers fall back to the driver's functions
void OnPresent();  // periodic summary
std::string Status();
}  // namespace ngx
