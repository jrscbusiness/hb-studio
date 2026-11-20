// miniaudio backed implementation of the RawDevice seam and of device enumeration.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "audio_io/ma_include.hpp"
#include "audio_io/raw_device.hpp"

namespace hb::audio_io {

const char* deviceKindName(DeviceKind k) noexcept {
    switch (k) {
        case DeviceKind::Capture: return "capture";
        case DeviceKind::Loopback: return "loopback";
        case DeviceKind::Playback: return "playback";
    }
    return "capture";
}

namespace {

// ---------------------------------------------------------------- backend selection
std::vector<ma_backend> backendList(Backend b) {
    if (b == Backend::Null) return {ma_backend_null};
#if defined(_WIN32)
    return {ma_backend_wasapi};  // loopback (system audio) exists only on WASAPI; ids stay consistent
#elif defined(__APPLE__)
    return {ma_backend_coreaudio};
#else
    return {ma_backend_pulseaudio, ma_backend_alsa};
#endif
}

const char* shortName(ma_backend b) {
    switch (b) {
        case ma_backend_wasapi: return "wasapi";
        case ma_backend_dsound: return "dsound";
        case ma_backend_winmm: return "winmm";
        case ma_backend_coreaudio: return "coreaudio";
        case ma_backend_sndio: return "sndio";
        case ma_backend_audio4: return "audio4";
        case ma_backend_oss: return "oss";
        case ma_backend_pulseaudio: return "pulse";
        case ma_backend_alsa: return "alsa";
        case ma_backend_jack: return "jack";
        case ma_backend_aaudio: return "aaudio";
        case ma_backend_opensl: return "opensl";
        case ma_backend_webaudio: return "webaudio";
        case ma_backend_null: return "null";
        default: return "other";
    }
}

const char* kindPrefix(DeviceKind k) {
    switch (k) {
        case DeviceKind::Capture: return "cap";
        case DeviceKind::Loopback: return "loop";
        case DeviceKind::Playback: return "play";
    }
    return "cap";
}

// ---------------------------------------------------------------- stable id encoding
// Payload is the backend's own identifier: plain text when the backend uses strings (and the text
// is printable ASCII), otherwise "x" + hex of the identifying bytes.
std::string hexOf(const unsigned char* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s = "x";
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

bool printableAscii(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e || c == ':') return false;  // ':' reserved as separator
    return true;
}

std::string encodePayload(ma_backend b, const ma_device_id& id) {
    auto cstr = [&](const char* p, std::size_t cap) {
        std::string s(p, strnlen(p, cap));
        return printableAscii(s) ? s : hexOf(reinterpret_cast<const unsigned char*>(p), s.size());
    };
    switch (b) {
        case ma_backend_wasapi: {
            std::string s;
            bool ascii = true;
            for (std::size_t i = 0; i < 64 && id.wasapi[i] != 0; ++i) {
                if (id.wasapi[i] >= 0x80) ascii = false;
                s.push_back(static_cast<char>(id.wasapi[i] & 0x7f));
            }
            if (ascii && printableAscii(s)) return s;
            std::size_t n = 0;
            while (n < 64 && id.wasapi[n] != 0) ++n;
            return hexOf(reinterpret_cast<const unsigned char*>(id.wasapi), n * sizeof(id.wasapi[0]));
        }
        case ma_backend_pulseaudio: return cstr(id.pulse, sizeof(id.pulse));
        case ma_backend_alsa: return cstr(id.alsa, sizeof(id.alsa));
        case ma_backend_coreaudio: return cstr(id.coreaudio, sizeof(id.coreaudio));
        case ma_backend_sndio: return cstr(id.sndio, sizeof(id.sndio));
        case ma_backend_audio4: return cstr(id.audio4, sizeof(id.audio4));
        case ma_backend_oss: return cstr(id.oss, sizeof(id.oss));
        case ma_backend_dsound: return hexOf(id.dsound, 16);
        case ma_backend_winmm: return hexOf(reinterpret_cast<const unsigned char*>(&id.winmm), 4);
        case ma_backend_null: {
            int v = id.nullbackend;
            return "n" + std::to_string(v);
        }
        default: return hexOf(reinterpret_cast<const unsigned char*>(&id), 4);
    }
}

std::string encodeId(DeviceKind kind, ma_backend b, const ma_device_id& id) {
    return std::string(kindPrefix(kind)) + ":" + shortName(b) + ":" + encodePayload(b, id);
}

bool unhex(const std::string& s, std::vector<unsigned char>& out) {
    if (s.size() < 3 || s[0] != 'x' || (s.size() - 1) % 2 != 0) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    out.clear();
    for (std::size_t i = 1; i + 1 < s.size() + 0; i += 2) {
        const int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<unsigned char>(a * 16 + b));
    }
    return true;
}

// Fills `id` from an id string produced by encodeId for backend `b`. Returns false when the string
// does not belong to this backend or is malformed. `useDefault` is set for ""/"<kind>:default".
bool decodeId(const std::string& text, ma_backend b, ma_device_id& id, bool& useDefault) {
    std::memset(&id, 0, sizeof(id));
    useDefault = false;
    if (text.empty()) { useDefault = true; return true; }
    const auto c1 = text.find(':');
    if (c1 == std::string::npos) return false;
    const auto c2 = text.find(':', c1 + 1);
    const std::string pre = text.substr(0, c1);
    if (pre != "cap" && pre != "loop" && pre != "play") return false;
    if (c2 == std::string::npos) {
        if (text.substr(c1 + 1) == "default") { useDefault = true; return true; }
        return false;
    }
    if (text.substr(c1 + 1, c2 - c1 - 1) != shortName(b)) return false;
    const std::string payload = text.substr(c2 + 1);
    if (payload.empty() || payload.size() > 600) return false;
    std::vector<unsigned char> bytes;
    const bool isHex = payload[0] == 'x' && unhex(payload, bytes);
    auto copyStr = [&](char* dst, std::size_t cap) {
        if (isHex) {
            if (bytes.size() >= cap) return false;
            std::memcpy(dst, bytes.data(), bytes.size());
        } else {
            if (payload.size() >= cap) return false;
            std::memcpy(dst, payload.data(), payload.size());
        }
        return true;
    };
    switch (b) {
        case ma_backend_wasapi:
            if (isHex) {
                if (bytes.size() >= sizeof(id.wasapi)) return false;
                std::memcpy(id.wasapi, bytes.data(), bytes.size());
            } else {
                if (payload.size() >= 64) return false;
                for (std::size_t i = 0; i < payload.size(); ++i)
                    id.wasapi[i] = static_cast<ma_wchar_win32>(static_cast<unsigned char>(payload[i]));
            }
            return true;
        case ma_backend_pulseaudio: return copyStr(id.pulse, sizeof(id.pulse));
        case ma_backend_alsa: return copyStr(id.alsa, sizeof(id.alsa));
        case ma_backend_coreaudio: return copyStr(id.coreaudio, sizeof(id.coreaudio));
        case ma_backend_sndio: return copyStr(id.sndio, sizeof(id.sndio));
        case ma_backend_audio4: return copyStr(id.audio4, sizeof(id.audio4));
        case ma_backend_oss: return copyStr(id.oss, sizeof(id.oss));
        case ma_backend_dsound:
            if (!isHex || bytes.size() != 16) return false;
            std::memcpy(id.dsound, bytes.data(), 16);
            return true;
        case ma_backend_winmm:
            if (!isHex || bytes.size() != 4) return false;
            std::memcpy(&id.winmm, bytes.data(), 4);
            return true;
        case ma_backend_null: {
            if (payload[0] != 'n' || payload.size() > 10) return false;
            int v = 0;
            for (std::size_t i = 1; i < payload.size(); ++i) {
                if (payload[i] < '0' || payload[i] > '9') return false;
                v = v * 10 + (payload[i] - '0');
            }
            id.nullbackend = v;
            return true;
        }
        default: return false;
    }
}

std::string describe(ma_result r) {
    const char* d = ma_result_description(r);
    return d ? d : "erro desconhecido";
}

// ---------------------------------------------------------------- context helper
struct Context {
    ma_context ctx{};
    bool ok = false;
    ~Context() {
        if (ok) ma_context_uninit(&ctx);
    }
    bool init(Backend b, std::string& error) {
        const auto list = backendList(b);
        ma_context_config cfg = ma_context_config_init();
        const ma_result r = ma_context_init(list.data(), static_cast<ma_uint32>(list.size()), &cfg, &ctx);
        if (r != MA_SUCCESS) {
            error = "Nenhum sistema de áudio disponível neste computador (" + describe(r) + ").";
            return false;
        }
        ok = true;
        return true;
    }
};

// ---------------------------------------------------------------- the device
class MaDevice final : public RawDevice {
public:
    MaDevice() = default;
    ~MaDevice() override {
        if (devOk_) ma_device_uninit(&dev_);  // blocks until the audio callback has returned
        if (ctx_.ok) ma_context_uninit(&ctx_.ctx), ctx_.ok = false;
    }

    bool open(const OpenRequest& req, std::string& error) {
        if (!ctx_.init(req.backend, error)) return false;
        ma_device_id id;
        bool useDefault = false;
        if (!decodeId(req.deviceId, ctx_.ctx.backend, id, useDefault)) {
            error = "Identificador de dispositivo inválido ou de outro sistema de áudio.";
            return false;
        }
        kind_ = req.kind;
        const ma_device_type type = req.kind == DeviceKind::Capture    ? ma_device_type_capture
                                    : req.kind == DeviceKind::Loopback ? ma_device_type_loopback
                                                                       : ma_device_type_playback;
        ma_device_config cfg = ma_device_config_init(type);
        cfg.sampleRate = 0;  // native rate of the device: no conversion in the OS layer
        cfg.periodSizeInMilliseconds = static_cast<ma_uint32>(std::clamp(req.periodMs, 2, 100));
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.noPreSilencedOutputBuffer = MA_TRUE;  // our callback always writes the whole buffer
        cfg.noClip = MA_TRUE;
        cfg.dataCallback = &MaDevice::dataCallback;
        cfg.notificationCallback = &MaDevice::notifyCallback;
        cfg.pUserData = this;
        if (req.kind == DeviceKind::Playback) {
            cfg.playback.format = ma_format_f32;
            cfg.playback.channels = 2;  // miniaudio converts channel layouts (mono/5.1... <-> stereo)
            cfg.playback.pDeviceID = useDefault ? nullptr : &id;
        } else {
            cfg.capture.format = ma_format_f32;
            cfg.capture.channels = 2;
            cfg.capture.pDeviceID = useDefault ? nullptr : &id;
        }
        const ma_result r = ma_device_init(&ctx_.ctx, &cfg, &dev_);
        if (r != MA_SUCCESS) {
            error = "Não foi possível abrir o dispositivo de áudio (" + describe(r) + ").";
            return false;
        }
        devOk_ = true;
        rate_ = static_cast<int>(dev_.sampleRate);
        chans_ = static_cast<int>(req.kind == DeviceKind::Playback ? dev_.playback.internalChannels
                                                                   : dev_.capture.internalChannels);
        if (rate_ <= 0) rate_ = 48000;
        if (chans_ <= 0) chans_ = 2;
        return true;
    }

    int sampleRate() const override { return rate_; }
    int nativeChannels() const override { return chans_; }

    bool start(RawCallbacks callbacks, std::string& error) override {
        cb_ = std::move(callbacks);
        const ma_result r = ma_device_start(&dev_);
        if (r != MA_SUCCESS) {
            error = "Não foi possível iniciar o dispositivo de áudio (" + describe(r) + ").";
            return false;
        }
        started_.store(true);
        return true;
    }

    bool alive() const override {
        if (!started_.load() || stoppedByDriver_.load()) return false;
        const ma_device_state s = ma_device_get_state(&dev_);
        return s == ma_device_state_started || s == ma_device_state_starting;
    }

private:
    static void dataCallback(ma_device* d, void* out, const void* in, ma_uint32 frames) {
        auto* self = static_cast<MaDevice*>(d->pUserData);
        if (!self) return;
        try {
            if (self->kind_ == DeviceKind::Playback) {
                if (self->cb_.playback) self->cb_.playback(static_cast<float*>(out), frames);
                else std::memset(out, 0, static_cast<std::size_t>(frames) * 2 * sizeof(float));
            } else if (self->cb_.capture && in) {
                self->cb_.capture(static_cast<const float*>(in), frames);
            }
        } catch (...) {
        }
    }
    static void notifyCallback(const ma_device_notification* n) {
        auto* self = static_cast<MaDevice*>(n->pDevice->pUserData);
        if (self && n->type == ma_device_notification_type_stopped) self->stoppedByDriver_.store(true);
    }

    Context ctx_;
    ma_device dev_{};
    bool devOk_ = false;
    DeviceKind kind_ = DeviceKind::Capture;
    int rate_ = 48000, chans_ = 2;
    RawCallbacks cb_;
    std::atomic<bool> started_{false}, stoppedByDriver_{false};
};

}  // namespace

DeviceOpener defaultDeviceOpener() {
    return [](const OpenRequest& req, std::string& error) -> std::unique_ptr<RawDevice> {
        try {
            auto d = std::make_unique<MaDevice>();
            if (!d->open(req, error)) return nullptr;
            return d;
        } catch (const std::exception& e) {
            error = std::string("Falha inesperada ao abrir o dispositivo: ") + e.what();
        } catch (...) {
            error = "Falha inesperada ao abrir o dispositivo.";
        }
        return nullptr;
    };
}

std::vector<DeviceInfo> listDevicesNative(Backend backend, std::string& error) {
    std::vector<DeviceInfo> out;
    try {
        Context c;
        if (!c.init(backend, error)) return out;
        ma_device_info* play = nullptr;
        ma_device_info* cap = nullptr;
        ma_uint32 nPlay = 0, nCap = 0;
        const ma_result r = ma_context_get_devices(&c.ctx, &play, &nPlay, &cap, &nCap);
        if (r != MA_SUCCESS) {
            error = "Não foi possível listar os dispositivos de áudio (" + describe(r) + ").";
            return out;
        }
        const bool canLoopback = c.ctx.backend == ma_backend_wasapi;
        auto detail = [&](ma_device_type t, const ma_device_info& base, DeviceInfo& di) {
            ma_device_info full{};
            if (ma_context_get_device_info(&c.ctx, t, &base.id, &full) == MA_SUCCESS &&
                full.nativeDataFormatCount > 0) {
                if (full.nativeDataFormats[0].channels > 0)
                    di.channels = static_cast<int>(full.nativeDataFormats[0].channels);
                if (full.nativeDataFormats[0].sampleRate > 0)
                    di.sampleRate = static_cast<int>(full.nativeDataFormats[0].sampleRate);
            }
        };
        for (ma_uint32 i = 0; i < nCap; ++i) {
            DeviceInfo di;
            di.kind = DeviceKind::Capture;
            di.name = cap[i].name;
            di.isDefault = cap[i].isDefault != 0;
            di.id = encodeId(DeviceKind::Capture, c.ctx.backend, cap[i].id);
            detail(ma_device_type_capture, cap[i], di);
            out.push_back(std::move(di));
        }
        for (ma_uint32 i = 0; i < nPlay; ++i) {
            DeviceInfo di;
            di.kind = DeviceKind::Playback;
            di.name = play[i].name;
            di.isDefault = play[i].isDefault != 0;
            di.id = encodeId(DeviceKind::Playback, c.ctx.backend, play[i].id);
            detail(ma_device_type_playback, play[i], di);
            if (canLoopback) {
                DeviceInfo lb = di;
                lb.kind = DeviceKind::Loopback;
                lb.id = encodeId(DeviceKind::Loopback, c.ctx.backend, play[i].id);
                lb.name = di.name + " (áudio do sistema)";
                out.push_back(std::move(lb));
            }
            out.push_back(std::move(di));
        }
    } catch (const std::exception& e) {
        error = std::string("Falha inesperada ao listar dispositivos: ") + e.what();
    } catch (...) {
        error = "Falha inesperada ao listar dispositivos.";
    }
    return out;
}

}  // namespace hb::audio_io
