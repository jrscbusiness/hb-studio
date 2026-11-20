#pragma once
// Thin, testable seam between the audio engine and the operating system's audio API.
//
// Everything above this header (CaptureStream, PlaybackStream, AudioEngine) is portable and
// hardware independent: it only talks to RawDevice. The default opener is implemented with
// miniaudio (raw_device_ma.cpp, WASAPI on Windows, PulseAudio/ALSA on Linux, "null" for tests);
// unit tests plug a fake opener to simulate devices appearing, vanishing and drifting.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hb::audio_io {

enum class DeviceKind { Capture, Loopback, Playback };
enum class Backend { Auto, Null };  // Auto = WASAPI / PulseAudio+ALSA / CoreAudio; Null = miniaudio null backend

const char* deviceKindName(DeviceKind k) noexcept;  // "capture" | "loopback" | "playback"

struct DeviceInfo {
    std::string id;       // stable across runs: "<kind>:<backend>:<payload>", e.g. "cap:wasapi:{0.0.1...}"
    std::string name;     // as reported by the OS
    DeviceKind kind = DeviceKind::Capture;
    bool isDefault = false;
    int channels = 2;     // native channel count (informational: streams always deliver stereo)
    int sampleRate = 48000;  // native rate
};

struct OpenRequest {
    DeviceKind kind = DeviceKind::Capture;
    std::string deviceId;  // "" or "<kind>:default" = system default
    Backend backend = Backend::Auto;
    int periodMs = 10;
};

// Callbacks run on the OS audio thread: no allocation, no locks, never throw.
struct RawCallbacks {
    std::function<void(const float* interleavedStereo, std::size_t frames)> capture;
    std::function<void(float* interleavedStereo, std::size_t frames)> playback;  // must fill everything
};

// A device that has been opened (native rate, f32, stereo after channel conversion) but not started.
class RawDevice {
public:
    virtual ~RawDevice() = default;
    virtual int sampleRate() const = 0;      // native rate: no sample-rate conversion is done by the OS layer
    virtual int nativeChannels() const = 0;  // before conversion to stereo
    // Installs the callbacks and starts the stream. Returns false and fills `error` (pt-BR) on failure.
    virtual bool start(RawCallbacks callbacks, std::string& error) = 0;
    // False once the stream has stopped by itself (device unplugged, driver reset...).
    virtual bool alive() const = 0;
};

// Returns nullptr and fills `error` (pt-BR) on failure. Must not throw.
using DeviceOpener = std::function<std::unique_ptr<RawDevice>(const OpenRequest&, std::string& error)>;

// miniaudio based opener (the default).
DeviceOpener defaultDeviceOpener();

// Lists devices of the given backend. Never throws; on failure returns what could be read and sets error.
std::vector<DeviceInfo> listDevicesNative(Backend backend, std::string& error);

}  // namespace hb::audio_io
