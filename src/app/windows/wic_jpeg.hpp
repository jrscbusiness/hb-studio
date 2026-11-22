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

// The reverse: JPEG bytes -> BGRA8, 4 bytes/pixel tightly packed (stride == width*4). Used when a
// capture device's DirectShow filter only offers MJPEG frames directly (some webcams/virtual
// cameras, e.g. DroidCam, do this and refuse any other connection) - DShowSource decodes each
// frame itself rather than relying on DirectShow's own MJPEG decompressor filter being installed.
class JpegDecoder {
public:
    bool init();
    // False on any failure (corrupt frame, size mismatch); out is resized to width*height*4 only
    // on success, so the caller's buffer is never left in a half-written state.
    bool decode(const std::uint8_t* jpeg, std::size_t len, int width, int height, std::vector<std::uint8_t>& out);

private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
};

}  // namespace hb::win
