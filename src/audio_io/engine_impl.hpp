#pragma once
// Internal data of AudioEngine (shared by audio_engine.cpp, engine_json.cpp, engine_settings.cpp).
#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "audio/delay_line.hpp"
#include "audio/drift_compensator.hpp"
#include "audio/meters.hpp"
#include "audio_io/audio_engine.hpp"
#include "audio_io/streams.hpp"

namespace hb::audio_io {

// Epoch gate for lock-free publication: the engine thread brackets each use of published
// pointers with enter()/exit(). A control thread that replaced a pointer takes mark() AFTER the
// swap; the old object may be freed once passed(mark) (no block that could still see it is running).
class EpochGate {
public:
    void enter() noexcept { seq_.fetch_add(1); }
    void exit() noexcept { seq_.fetch_add(1); }
    std::uint64_t mark() const noexcept { return seq_.load(); }
    bool passed(std::uint64_t m) const noexcept { return (m & 1u) == 0 || seq_.load() != m; }

private:
    std::atomic<std::uint64_t> seq_{0};
};

struct Slot {  // one effect of a chain; shared between consecutive chain snapshots
    std::shared_ptr<hb::audio::Effect> fx;  // null when the effect is missing
    std::string typeId, name;
    bool missing = false;
    std::atomic<bool> bypass{false};
    std::atomic<float> gr{0.0f};                       // cached by the engine thread
    std::vector<std::pair<std::string, float>> saved;  // params kept for missing effects
};

struct Chain {  // immutable once published
    std::vector<std::shared_ptr<Slot>> slots;
};

struct InputNode {  // owned by the engine thread while published (pull state), built by control threads
    std::shared_ptr<CaptureStream> stream;
    std::shared_ptr<CaptureSession> session;
    std::unique_ptr<hb::audio::DriftCompensator> drift;
    std::shared_ptr<hb::audio::AudioSource> source;
    std::unique_ptr<hb::audio::Resampler> resampler;
    void pull(float* out, std::size_t frames) noexcept;
};

struct StripRT {
    explicit StripRT(int sr);
    std::atomic<float> trim{1.0f};
    std::atomic<bool> phase{false}, mono{false}, toProgram{true}, toMonitor{false};
    std::atomic<InputNode*> input{nullptr};
    std::atomic<const Chain*> chain{nullptr};
    hb::audio::DelayLine delay;
    // engine thread only
    bool init = false;
    float trimCur = 1.0f, monoCur = 0.0f;
    hb::audio::PeakMeter peak;
    hb::audio::RmsMeter rms;
    hb::audio::LoudnessMeter loud;
    std::vector<float> buf;
    // published levels: peakL peakR holdL holdR rmsL rmsR lufsS gr
    std::array<std::atomic<float>, 8> pub;
};

struct StripCtl {  // control-side configuration, guarded by Impl::ctl
    std::string name;
    InputKind kind = InputKind::None;
    std::string deviceId, externalId;
    float trimDb = 0, faderDb = 0;
    bool mute = false, solo = false;
    float pan = 0;
    bool phase = false, mono = false;
    float delayMs = 0;
    Quality quality = Quality::Medium;
    bool toProgram = true, toMonitor = false;
    std::shared_ptr<CaptureStream> capture;
    std::shared_ptr<InputNode> node;
    std::shared_ptr<Chain> chain;
};

struct ConsumerEntry {
    AudioEngine::ConsumerId id = 0;
    AudioEngine::ProgramConsumer fn;
};
struct ConsumerList {
    std::vector<std::shared_ptr<ConsumerEntry>> items;
};

struct AudioEngine::Impl {
    explicit Impl(EngineOptions o);

    EngineOptions opt;
    int n;
    hb::audio::Mixer mixer;
    DeviceEnumerator enumerator;

    mutable std::mutex ctl;  // control side
    std::vector<std::unique_ptr<StripRT>> rt;
    std::vector<StripCtl> strips;
    std::map<std::string, std::shared_ptr<hb::audio::AudioSource>> externals;
    struct {
        bool limiter = true;
        float faderDb = 0, ceilingDb = -1;
    } master;
    struct {
        std::string deviceId;
        float level = 0.75f;
        bool mute = false;
        std::shared_ptr<PlaybackStream> stream;
    } mon;
    std::atomic<PlaybackStream*> monStream{nullptr};
    std::atomic<float> monGain{1.0f};
    std::atomic<bool> monMute{false};
    float monCur = 1.0f;
    bool monInit = false;
    hb::audio::PeakMeter monPeak;
    std::array<std::atomic<float>, 2> monPub;
    std::vector<float> monBuf, prog;

    std::atomic<unsigned> version{0};
    mutable std::mutex errMutex;
    std::string lastError;
    mutable std::mutex warnMutex;
    std::vector<std::string> warnings;

    // reclamation
    EpochGate gate;
    std::mutex retMutex;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const void>>> retired;
    void retire(std::shared_ptr<const void> p);
    void collect();

    // consumers
    std::mutex consMutex;
    std::shared_ptr<ConsumerList> consOwner;
    std::atomic<const ConsumerList*> consPub{nullptr};
    std::atomic<std::thread::id> engineThreadId{};
    AudioEngine::ConsumerId nextConsumer = 1;

    // stats
    std::atomic<std::uint64_t> blocks{0}, xruns{0}, consumerErrors{0};
    std::atomic<double> cpu{0}, cpuPeak{0};

    // threads
    std::mutex runMutex;
    std::thread engineThread, maintThread;
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> isRunning{false};

    // helpers (all expect `ctl` held unless stated)
    bool fail(const std::string& msg);
    bool checkStrip(int i);
    void rebuildInput(int i);
    void publishChain(int i, std::shared_ptr<Chain> c);
    void applyStripToRt(int i);
    void engineLoop();
    void maintLoop();
};

void processBlockImpl(AudioEngine::Impl& impl);  // one 10 ms block
void maintenanceStep(AudioEngine::Impl& impl);   // control-side upkeep
const char* defaultStripName(int index);
std::string sanitizeName(const std::string& s, std::size_t maxBytes);
float clampDb(double v, float lo, float hi);  // <= -100 -> -inf; NaN must be rejected by the caller

}  // namespace hb::audio_io
