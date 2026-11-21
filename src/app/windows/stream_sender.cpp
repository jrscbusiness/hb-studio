#include "app/windows/stream_sender.hpp"

#include "audio/audio_common.hpp"
#include "streaming/flv.hpp"
#include "streaming/rtmp_client.hpp"

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
    std::lock_guard lock(statusMutex_);
    return status_;
}

void StreamSender::start() {
    if (thread_.joinable()) return;   // already running
    stopFlag_ = false;
    {
        std::lock_guard lock(statusMutex_);
        status_ = Status{};
        status_.running = true;
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
    setStatus(false, false, status().message);
}

void StreamSender::run() {
    const auto target = streaming::parseRtmpUrl(dest_.serverUrl);
    if (!target.ok) {
        setStatus(false, false, "Endereço do servidor inválido: \"" + dest_.serverUrl + "\".");
        return;
    }

    // Encoders are created and started BEFORE the network connection: there is no point opening a
    // real connection to the operator's own YouTube/Twitch/etc. account (which shows up as a
    // "viewer"/connection attempt on their dashboard) if nothing can ever be sent over it. Once a
    // real Media Foundation implementation replaces today's placeholder (see
    // src/streaming/windows/mf_video_encoder.cpp), this same code starts working end to end with no
    // changes needed here.
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

    setStatus(true, false, "Conectando a " + target.host + "…");
    hb::streaming::RtmpTarget rt;
    rt.host = target.host;
    rt.port = target.port;
    rt.app = target.app;
    rt.streamKey = dest_.streamKey;
    hb::streaming::RtmpClient rtmp;
    if (!rtmp.start(rt, 8000)) {
        videoEnc->stop();
        audioEnc->stop();
        setStatus(false, false, rtmp.lastError());
        return;
    }

    vConsumer_ = program_.addConsumer([this](const std::shared_ptr<const video::Frame>& f) {
        std::lock_guard lock(queueMutex_);
        if (videoQueue_.size() >= kMaxQueuedVideo) videoQueue_.pop_front();
        videoQueue_.push_back(f);
        queueCv_.notify_one();
    });
    aConsumer_ = audio_.addProgramConsumer([this](const float* s, std::size_t frames) {
        std::lock_guard lock(queueMutex_);
        if (audioQueue_.size() >= kMaxQueuedAudio) audioQueue_.pop_front();
        audioQueue_.emplace_back(s, s + frames * static_cast<std::size_t>(audioChannels_));
        queueCv_.notify_one();
    });

    setStatus(true, true, "Transmitindo para " + (dest_.name.empty() ? target.host : dest_.name) + ".");
    const auto start = std::chrono::steady_clock::now();
    bool sentVideoHeader = false, sentAudioHeader = false;
    std::uint64_t videoUnits = 0, audioUnits = 0;

    while (!stopFlag_) {
        std::shared_ptr<const video::Frame> vFrame;
        std::vector<float> aBlock;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait_for(lock, std::chrono::milliseconds(200),
                              [&] { return stopFlag_.load() || !videoQueue_.empty() || !audioQueue_.empty(); });
            if (!videoQueue_.empty()) { vFrame = videoQueue_.front(); videoQueue_.pop_front(); }
            if (!audioQueue_.empty()) { aBlock = std::move(audioQueue_.front()); audioQueue_.pop_front(); }
        }
        bool ok = true;
        if (vFrame) {
            const std::uint32_t ts = msFromStart(start);
            ok = ok && videoEnc->encode(vFrame->data.data(), vFrame->width, vFrame->height, vFrame->stride, ts,
                                        [&](const streaming::EncodedVideoUnit& u) {
                                            if (!sentVideoHeader) {
                                                std::vector<std::uint8_t> sps, pps;
                                                if (videoEnc->spsPps(sps, pps)) {
                                                    rtmp.sendVideo(u.timestampMs, streaming::flv::buildAvcSequenceHeader(sps, pps));
                                                    sentVideoHeader = true;
                                                }
                                            }
                                            rtmp.sendVideo(u.timestampMs, streaming::flv::buildAvcVideoTag({u.nalu}, u.keyframe));
                                            ++videoUnits;
                                        });
        }
        if (ok && !aBlock.empty()) {
            const std::uint32_t ts = msFromStart(start);
            const std::size_t frames = aBlock.size() / static_cast<std::size_t>(audioChannels_);
            ok = audioEnc->encode(aBlock.data(), frames, ts, [&](const streaming::EncodedAudioUnit& u) {
                if (!sentAudioHeader) {
                    std::vector<std::uint8_t> asc;
                    if (audioEnc->audioSpecificConfig(asc)) {
                        rtmp.sendAudio(u.timestampMs, streaming::flv::buildAacSequenceHeader(asc));
                        sentAudioHeader = true;
                    }
                }
                rtmp.sendAudio(u.timestampMs, streaming::flv::buildAacAudioTag(u.raw));
                ++audioUnits;
            });
        }
        if (!ok || !rtmp.isPublishing()) {
            setStatus(true, false,
                     "Conexão perdida" + (rtmp.lastError().empty() ? std::string(".") : (": " + rtmp.lastError())));
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
    rtmp.stop();
}

}  // namespace hb::win
