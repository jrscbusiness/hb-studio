#pragma once
// Wire protocol between the HB Studio app (producer) and the virtual camera
// media source DLL (consumer), which runs inside the Windows Frame Server
// service. The DLL hosts a named-pipe server; the app connects and writes
// frames. A pipe is used (instead of shared memory) because the service runs
// in session 0 and a non-elevated app cannot create Global\ shared sections.
#include <cstdint>

namespace hb::vcam {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\HBStudioVirtualCamera";

// CLSID of the media source activator registered under HKLM.
inline constexpr wchar_t kMediaSourceClsidString[] = L"{6F0F5B0E-3A52-4D7C-9B2B-5E0C7A1D4C31}";

// Fixed output format of the PoC camera. The Program Output must match.
inline constexpr int kWidth = 1920;
inline constexpr int kHeight = 1080;
inline constexpr int kFpsNum = 30;
inline constexpr int kFpsDen = 1;

inline constexpr std::uint32_t kMagic = 0x43564248;  // "HBVC"

#pragma pack(push, 1)
struct FrameHeader {
    std::uint32_t magic;         // kMagic
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;        // bytes per row, BGRA8
    std::uint32_t payloadBytes;  // == stride * height
};
#pragma pack(pop)

}  // namespace hb::vcam
