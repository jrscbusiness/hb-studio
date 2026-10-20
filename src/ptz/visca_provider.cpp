#include "ptz/visca_provider.hpp"

#include <cmath>
#include <exception>

namespace hb::ptz {

using visca::Message;

namespace {
template <class F>
PtzResult safely(F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        return PtzResult::error(std::string("exception: ") + e.what());
    } catch (...) {
        return PtzResult::error("unknown exception");
    }
}
}  // namespace

ViscaProvider::ViscaProvider(std::unique_ptr<ITransport> transport, ViscaConfig cfg)
    : t_(std::move(transport)), cfg_(cfg) {
    if (cfg_.overIp) cfg_.address = 1;
    caps_.panTilt = caps_.zoom = caps_.variableSpeed = true;
    caps_.presetRecall = caps_.presetSave = caps_.presetDelete = true;
    caps_.presetList = false;  // VISCA cannot enumerate presets
    caps_.focusAuto = caps_.focusManual = caps_.focusDrive = caps_.iris = true;
    caps_.statusInquiry = caps_.absoluteMove = true;
    caps_.maxPresetId = cfg_.maxPresetId;
}

PtzResult ViscaProvider::exchange(const Bytes& payload, bool isInquiry, Message* reply) {
    if (!t_ || !t_->isOpen()) return PtzResult::error("not connected");
    Bytes datagram = payload;
    if (cfg_.overIp)
        datagram = visca::wrapIp(isInquiry ? visca::IpType::Inquiry : visca::IpType::Command, seq_++, payload);
    if (auto r = t_->send(datagram); !r.ok) return PtzResult::error("send: " + r.error);

    for (int i = 0; i < cfg_.maxReadsPerCommand; ++i) {
        Bytes chunk;
        auto r = t_->receive(chunk, cfg_.ackTimeout);
        if (r.timedOut) return PtzResult::timeout("no VISCA reply");
        if (!r.ok) return PtzResult::error("receive: " + r.error);
        if (cfg_.overIp) {
            auto f = visca::unwrapIp(chunk);
            if (!f || f->type != static_cast<std::uint16_t>(visca::IpType::Reply)) continue;
            chunk = std::move(f->payload);
        }
        stream_.insert(stream_.end(), chunk.begin(), chunk.end());
        for (const auto& frame : visca::extractFrames(stream_)) {
            auto m = visca::parseMessage(frame);
            if (!m) continue;
            if (m->kind == Message::Kind::Error)
                return PtzResult::error(std::string("VISCA: ") + visca::errorName(m->errorCode));
            if (!isInquiry && m->kind == Message::Kind::Ack) return PtzResult::success();
            if (isInquiry && m->kind == Message::Kind::InquiryReply) {
                if (reply) *reply = std::move(*m);
                return PtzResult::success();
            }
            // Completion of an earlier command or unknown frame: ignore.
        }
    }
    return PtzResult::timeout("no matching VISCA reply");
}

PtzResult ViscaProvider::command(const Bytes& p) { return exchange(p, false, nullptr); }
PtzResult ViscaProvider::inquiry(const Bytes& p, Message& r) { return exchange(p, true, &r); }

PtzResult ViscaProvider::connect() {
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!t_) return PtzResult::error("no transport");
        if (auto r = t_->open(); !r.ok) return PtzResult::error("open: " + r.error);
        connected_ = true;
        stream_.clear();
        lastPT_ = std::pair<double, double>{0, 0};  // assume idle; Stop is always forced on disconnect
        lastZoom_ = 0.0;
        seq_ = 0;
        if (cfg_.overIp && cfg_.sequenceReset) {
            // Best effort: some cameras do not answer the control message.
            if (t_->send(visca::ipSequenceReset(0)).ok) {
                Bytes junk;
                t_->receive(junk, cfg_.ackTimeout);
            }
            seq_ = 1;
        }
        return PtzResult::success();
    });
}

PtzResult ViscaProvider::disconnect() {
    PtzResult stopped = PtzResult::success();
    if (connected_) stopped = stop();  // guaranteed stop before closing
    return safely([&] {
        std::lock_guard lk(mu_);
        if (t_) t_->close();
        connected_ = false;
        return stopped;
    });
}

PtzResult ViscaProvider::drive(const MoveVector& v) {
    return safely([&] {
        std::lock_guard lk(mu_);
        PtzResult res;
        const std::pair<double, double> pt{v.pan, v.tilt};
        if (!lastPT_ || *lastPT_ != pt) {
            res = command(visca::panTiltDrive(cfg_.address, v.pan, v.tilt));
            if (res.ok()) lastPT_ = pt;
        }
        if (!lastZoom_ || *lastZoom_ != v.zoom) {
            auto z = command(visca::zoomDrive(cfg_.address, v.zoom));
            if (z.ok()) lastZoom_ = v.zoom;
            if (res.ok()) res = z;
        }
        return res;
    });
}

PtzResult ViscaProvider::stopAll() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto a = command(visca::panTiltStop(cfg_.address));
        auto b = command(visca::zoomDrive(cfg_.address, 0));
        if (a.ok()) lastPT_ = std::pair<double, double>{0, 0};
        if (b.ok()) lastZoom_ = 0.0;
        return a.ok() ? b : a;
    });
}

PtzResult ViscaProvider::stopLens() {
    return safely([&] {
        std::lock_guard lk(mu_);
        if (!lensActive_) return PtzResult::success();
        auto r = command(visca::focusStop(cfg_.address));
        if (r.ok()) lensActive_ = false;
        return r;
    });
}

PtzResult ViscaProvider::moveAbsolute(const PtzPosition& p) {
    if (p.units != PositionUnits::ViscaRaw) return PtzResult::error("VISCA needs raw position units");
    return safely([&] {
        std::lock_guard lk(mu_);
        auto a = command(visca::panTiltAbsolute(cfg_.address, 0.6, 0.6, static_cast<int>(std::lround(p.pan)),
                                                static_cast<int>(std::lround(p.tilt))));
        if (!a.ok()) return a;
        lastPT_.reset();
        lastZoom_.reset();
        return command(visca::zoomDirect(cfg_.address, static_cast<int>(std::lround(p.zoom))));
    });
}

PtzResult ViscaProvider::checkPreset(int id) const {
    if (id < 0 || id > cfg_.maxPresetId) return PtzResult::error("preset id out of range");
    return PtzResult::success();
}

PtzResult ViscaProvider::gotoPreset(int id) {
    if (auto c = checkPreset(id); !c.ok()) return c;
    return safely([&] { std::lock_guard lk(mu_); lastPT_.reset(); lastZoom_.reset(); return command(visca::presetRecall(cfg_.address, id)); });
}
PtzResult ViscaProvider::savePreset(int id) {
    if (auto c = checkPreset(id); !c.ok()) return c;
    return safely([&] { std::lock_guard lk(mu_); return command(visca::presetSet(cfg_.address, id)); });
}
PtzResult ViscaProvider::deletePreset(int id) {
    if (auto c = checkPreset(id); !c.ok()) return c;
    return safely([&] { std::lock_guard lk(mu_); return command(visca::presetReset(cfg_.address, id)); });
}
PtzReply<std::vector<PresetInfo>> ViscaProvider::getPresets() {
    return {PtzResult::notSupported("VISCA cannot list presets"), {}};
}

PtzResult ViscaProvider::focusAuto() {
    return safely([&] { std::lock_guard lk(mu_); return command(visca::focusAuto(cfg_.address)); });
}
PtzResult ViscaProvider::focusManual() {
    return safely([&] { std::lock_guard lk(mu_); return command(visca::focusManual(cfg_.address)); });
}
PtzResult ViscaProvider::focusNear() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = command(visca::focusNear(cfg_.address, 0.5));
        if (r.ok()) lensActive_ = true;
        return r;
    });
}
PtzResult ViscaProvider::focusFar() {
    return safely([&] {
        std::lock_guard lk(mu_);
        auto r = command(visca::focusFar(cfg_.address, 0.5));
        if (r.ok()) lensActive_ = true;
        return r;
    });
}
PtzResult ViscaProvider::irisOpen() {
    return safely([&] { std::lock_guard lk(mu_); return command(visca::irisUp(cfg_.address)); });
}
PtzResult ViscaProvider::irisClose() {
    return safely([&] { std::lock_guard lk(mu_); return command(visca::irisDown(cfg_.address)); });
}

PtzReply<CameraStatus> ViscaProvider::getCameraStatus() {
    PtzReply<CameraStatus> out;
    out.result = safely([&] {
        std::lock_guard lk(mu_);
        out.value.provider = name();
        out.value.connected = connected_;
        Message pt, z;
        if (auto r = inquiry(visca::inquiryPanTiltPosition(cfg_.address), pt); !r.ok()) return r;
        if (auto r = inquiry(visca::inquiryZoomPosition(cfg_.address), z); !r.ok()) return r;
        int pan = 0, tilt = 0, zoom = 0;
        if (!visca::parsePanTiltPosition(pt, pan, tilt) || !visca::parseZoomPosition(z, zoom))
            return PtzResult::error("malformed position reply");
        out.value.position = PtzPosition{double(pan), double(tilt), double(zoom), PositionUnits::ViscaRaw};
        return PtzResult::success();
    });
    return out;
}

}  // namespace hb::ptz
