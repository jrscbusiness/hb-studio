#include "app/windows/stream_sender.hpp"

#include <algorithm>

#include "audio/audio_common.hpp"
#include "streaming/adaptive_bitrate.hpp"
#include "streaming/flv.hpp"
#include "streaming/reconnecting_publisher.hpp"
#include "streaming/rtmp_client.hpp"
#include "streaming/transport.hpp"

namespace hb::win {

namespace {
std::uint32_t msFromStart(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint32_t>(
        std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
}
}  // namespace

StreamSender::StreamSender(hb::app::StreamDestination dest, video::ProgramOutput& program, audio_io::AudioEngine& audio)
    : dest_(std::move(dest)), program_(program), audio_(audio) {}

StreamSender::~StreamSender() { stop(); }

void StreamSender::setStatus(bool running, bool publishing, std::string message) {
    std::lock_guard lock(statusMutex_);
    status_.running = running;
    status_.publishing = publishing;
    status_.message = std::move(message);
}

StreamSender::Status StreamSender::status() const {
    Status s;
    {
        std::lock_guard lock(statusMutex_);
        s = status_;
    }
    s.stats = stats_.snapshot();
    return s;
}

void StreamSender::start() {
    if (thread_.joinable()) return;   // already running
    stopFlag_ = false;
    droppedVideoTotal_ = 0;
    stats_.reset();
    {
        std::lock_guard lock(statusMutex_);
        status_ = Status{};
        status_.running = true;
        status_.adaptive = dest_.adaptiveBitrate;
        status_.startedAt = std::chrono::steady_clock::now();
    }
    thread_ = std::thread([this] { run(); });
}

void StreamSender::stop() {
    stopFlag_ = true;
    queueCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (vConsumer_) { program_.removeConsumer(vConsumer_); vConsumer_ = 0; }
    if (aConsumer_) { audio_.removeProgramConsumer(aConsumer_); aConsumer_ = 0; }
    {
        std::lock_guard lock(queueMutex_);
        videoQueue_.clear();
        audioQueue_.clear();
    }
    setStatus(false, false, status().message);
}

void StreamSender::run() {
    // Which wire protocol: the URL scheme wins (rtmps:// is always TLS); a destination whose protocol
    // says rtmps but whose URL still says rtmp:// is upgraded (default port 443 unless spelled out).
    std::string url = dest_.serverUrl;
    if (dest_.protocol == "srt") {
        std::string why;
        streaming::makeStreamTransport("srt", &why);
        setStatus(false, false, why);
        return;
    }
    const auto target = streaming::parseRtmpUrl(url);
    if (!target.ok) {
        setStatus(false, false, "Endereço do servidor inválido: \"" + dest_.serverUrl + "\".");
        return;
    }
    const bool tls = target.tls || dest_.protocol == "rtmps";
    if (tls && !target.tls) {
        url = "rtmps://" + target.host + ":" + std::to_string(target.portExplicit ? target.port : 443) + "/" + target.app;
    }
    const std::string proto = tls ? "rtmps" : "rtmp";

    // Encoders are created and started BEFORE the network connection: there is no point opening a
    // real connection to the operator's own account if nothing can ever be sent over it.
    const auto pf = program_.format();
    const int w = dest_.useOwnFormat ? dest_.width : pf.width;
    const int h = dest_.useOwnFormat ? dest_.height : pf.height;
    const int fn = dest_.useOwnFormat ? dest_.fpsNum : pf.fps.num;
    const int fd = dest_.useOwnFormat ? dest_.fpsDen : pf.fps.den;

    auto videoEnc = makeH264Encoder();
    if (!videoEnc->start(w, h, fn, fd, dest_.videoKbps, dest_.encoder)) {
        setStatus(false, false, "Vídeo: " + videoEnc->lastError());
        return;
    }
    auto audioEnc = makeAacEncoder();
    if (!audioEnc->start(audio::kProgramSampleRate, audioChannels_, dest_.audioKbps)) {
        videoEnc->stop();
        setStatus(false, false, "Áudio: " + audioEnc->lastError());
        return;
    }
    stats_.setTargetKbps(dest_.videoKbps + dest_.audioKbps);
    stats_.setEncoderKbps(dest_.videoKbps + dest_.audioKbps);
    stats_.setEncoderName(videoEnc->activeEncoderName());

    streaming::ReconnectingPublisher::Options po;
    po.url = url;
    po.streamKey = dest_.streamKey;
    streaming::ReconnectingPublisher pub(
        po, [proto] { return streaming::makeStreamTransport(proto); }, &stats_);

    vConsumer_ = program_.addConsumer([this](const std::shared_ptr<const video::Frame>& f) {
        std::lock_guard lock(queueMutex_);
        if (videoQueue_.size() >= kMaxQueuedVideo) {
            videoQueue_.pop_front();
            ++droppedVideoTotal_;
            stats_.onDropped(true);
        }
        videoQueue_.push_back(f);
        queueCv_.notify_one();
    });
    aConsumer_ = audio_.addProgramConsumer([this](const float* s, std::size_t frames) {
        std::lock_guard lock(queueMutex_);
        if (audioQueue_.size() >= kMaxQueuedAudio) {
            audioQueue_.pop_front();
            stats_.onDropped(false);
        }
        audioQueue_.emplace_back(s, s + frames * static_cast<std::size_t>(audioChannels_));
        queueCv_.notify_one();
    });

    streaming::AbrConfig ac;
    ac.targetKbps = dest_.videoKbps;
    streaming::AdaptiveBitrate abr(ac);
    bool abrActive = dest_.adaptiveBitrate;

    const std::string label = dest_.name.empty() ? target.host : dest_.name;
    const auto start = std::chrono::steady_clock::now();
    auto lastTick = start;
    unsigned gen = 0;
    bool sentVideoHeader = false, sentAudioHeader = false, waitKey = true;
    std::uint64_t videoUnits = 0, audioUnits = 0;
    setStatus(true, false, "Conectando a " + target.host + "…");

    while (!stopFlag_) {
        const bool connected = pub.poll();
        if (pub.gaveUp()) {
            setStatus(false, false, pub.lastError());
            break;
        }
        {
            std::lock_guard lock(statusMutex_);
            status_.running = true;
            status_.publishing = connected;
            status_.reconnecting = !connected && pub.attempts() > 0;
            if (connected) {
                status_.message = "Transmitindo para " + label + ".";
            } else if (pub.attempts() > 0) {
                status_.message = "Reconectando em " + std::to_string((pub.msUntilRetry() + 999) / 1000) + " s (tentativa " +
                                  std::to_string(pub.attempts()) + "): " + pub.lastError();
            } else {
                status_.message = "Conectando a " + target.host + "…";
            }
        }

        std::shared_ptr<const video::Frame> vFrame;
        std::vector<float> aBlock;
        int qv = 0, qa = 0;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait_for(lock, std::chrono::milliseconds(connected ? 200 : 100),
                              [&] { return stopFlag_.load() || !videoQueue_.empty() || !audioQueue_.empty(); });
            if (!videoQueue_.empty()) { vFrame = videoQueue_.front(); videoQueue_.pop_front(); }
            if (!audioQueue_.empty()) { aBlock = std::move(audioQueue_.front()); audioQueue_.pop_front(); }
            qv = static_cast<int>(videoQueue_.size());
            qa = static_cast<int>(audioQueue_.size());
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastTick >= std::chrono::seconds(1)) {
            lastTick = now;
            stats_.setQueueDepth(qv, qa);
            if (abrActive && connected) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                streaming::AbrSample sample;
                sample.queueDepth = qv;
                sample.queueCapacity = static_cast<int>(kMaxQueuedVideo);
                sample.droppedTotal = droppedVideoTotal_.load();
                const int nb = abr.update(ms, sample);
                if (nb > 0) {
                    if (videoEnc->setBitrateKbps(nb)) {
                        stats_.setEncoderKbps(nb + dest_.audioKbps);
                    } else {
                        abrActive = false;   // this encoder cannot change bitrate live: stop trying
                        stats_.setLastError("Este codificador não permite mudar o bitrate ao vivo; bitrate adaptativo desativado.");
                    }
                }
            }
        }

        if (!connected) continue;   // frames popped above are discarded: nothing to send them on
        if (pub.generation() != gen) {   // fresh session: codec headers again, and start on a keyframe
            gen = pub.generation();
            sentVideoHeader = sentAudioHeader = false;
            waitKey = true;
            videoEnc->requestKeyFrame();
        }

        bool ok = true;
        if (vFrame) {
            const std::uint32_t ts = msFromStart(start);
            ok = videoEnc->encode(vFrame->data.data(), vFrame->width, vFrame->height, vFrame->stride, ts,
                                  [&](const EncodedVideoUnit& u) {
                                      if (waitKey && !u.keyframe) return;
                                      if (!sentVideoHeader) {
                                          std::vector<std::uint8_t> sps, pps;
                                          if (!videoEnc->spsPps(sps, pps)) return;
                                          if (!pub.sendVideo(u.timestampMs, streaming::flv::buildAvcSequenceHeader(sps, pps))) return;
                                          sentVideoHeader = true;
                                      }
                                      waitKey = false;
                                      if (pub.sendVideo(u.timestampMs, streaming::flv::buildAvcVideoTag({u.nalu}, u.keyframe))) ++videoUnits;
                                  });
        }
        if (ok && !aBlock.empty()) {
            const std::uint32_t ts = msFromStart(start);
            const std::size_t frames = aBlock.size() / static_cast<std::size_t>(audioChannels_);
            ok = audioEnc->encode(aBlock.data(), frames, ts, [&](const EncodedAudioUnit& u) {
                if (!sentAudioHeader) {
                    std::vector<std::uint8_t> asc;
                    if (!audioEnc->audioSpecificConfig(asc)) return;
                    if (!pub.sendAudio(u.timestampMs, streaming::flv::buildAacSequenceHeader(asc))) return;
                    sentAudioHeader = true;
                }
                if (pub.sendAudio(u.timestampMs, streaming::flv::buildAacAudioTag(u.raw))) ++audioUnits;
            });
        }
        if (!ok) {   // hard encoder failure: reconnecting cannot fix that
            setStatus(false, false,
                      "Codificador falhou: " + (videoEnc->lastError().empty() ? audioEnc->lastError() : videoEnc->lastError()));
            break;
        }
        {
            std::lock_guard lock(statusMutex_);
            status_.videoUnits = videoUnits;
            status_.audioUnits = audioUnits;
        }
    }

    videoEnc->stop();
    audioEnc->stop();
    pub.stop();
}

}  // namespace hb::win
