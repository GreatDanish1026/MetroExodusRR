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
void LoadSettings();  // ReShade.ini [MetroExodusRR]
void DrawSettings();  // the user's settings
void DrawAdvanced();  // testing tools

// Ray Reconstruction guides made at the RT composite pass (render size, native ID3D12Resource*),
// and the camera of that frame as NGX wants it (row-major, row vectors).
struct Guides {
    void* diffuse;
    void* specular;
    void* normals;  // world normal, roughness in w
    float worldToView[16];
    float viewToClip[16];
    uint64_t seq;  // increments with each new set
};
bool LatestGuides(Guides* out);
bool RrWanted();              // overlay switch
int RrPreset();               // RR model hint (0 default, 4 D, 5 E, 6 F)
void SetRrWorking(bool on);   // rr.cpp: RR ran this frame (turns the game's GI denoiser off)
}  // namespace rt

namespace ngx {
void Install();    // hooks GetProcAddress in MetroExodus.exe's import table
void Uninstall();  // restores it; the game's NGX pointers fall back to the driver's functions
void OnPresent();  // periodic summary
std::string Status();
enum DriverFn { kDriverCreate, kDriverEvaluate, kDriverRelease };
void* DriverFunction(int which);       // the driver's Create/Evaluate/Release the game looked up
void* DriverProc(const char* name);    // any other NVSDK_NGX_D3D12_* of the driver's NGX module
unsigned GameCreateFlags();            // the game's DLSS create flags
int GameQuality();                     // the game's DLSS PerfQualityValue
}  // namespace ngx

// Ray Reconstruction in place of the game's DLSS Super Resolution (rr.cpp).
namespace rr {
bool Evaluate(void* commandList, const void* gameParams);  // true = RR ran, skip the game's call
void OnGameRelease();
std::string Status();
}  // namespace rr
