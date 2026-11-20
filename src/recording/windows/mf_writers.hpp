#pragma once
#include <string>
#include <vector>

#include "recording/media_writer.hpp"

namespace hb::rec {

// Windows only. Writers built on the Media Foundation Sink Writer (encoders that ship with Windows or come
// with the GPU driver): MP4 H.264+AAC, MP4 HEVC+AAC, M4A (AAC) and MP3. A profile is registered only when its
// encoder is really present on this PC (Windows "N" editions and some servers lack some of them).
//
// Registers into WriterRegistry::instance(); safe to call more than once. Returns the ids registered.
std::vector<std::string> registerWindowsWriters();

// Which Media Foundation encoders were found (for diagnostics / the UI).
struct EncoderAvailability {
    bool h264 = false, hevc = false, aac = false, mp3 = false;
};
EncoderAvailability probeEncoders();

}  // namespace hb::rec
