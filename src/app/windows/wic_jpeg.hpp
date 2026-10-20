#pragma once
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "video/program_output.hpp"

namespace hb::win {

// BGRA8 frame -> JPEG bytes using the Windows Imaging Component.
// One instance per thread (create after CoInitializeEx on that thread).
class JpegEncoder {
public:
    bool init();
    bool encode(const video::Frame& frame, int quality, std::vector<std::uint8_t>& out);

private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
};

}  // namespace hb::win
