#pragma once
// Energy-based voice-activity segmenter. Fed 16 kHz mono samples, it emits utterances:
//   * Partial events (the audio of the utterance so far) every partialIntervalMs while speech continues,
//   * a Final event when speech ends (hangover of silence), or when the utterance gets too long.
// Time is the sample timeline (position = samples fed / rate): no wall clock, fully deterministic.
// The noise floor adapts during silence, so the speech threshold follows the room.
// This is a plain RMS gate, NOT a neural VAD: steady loud noise/music will be treated as speech.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace hb::captions {

struct SegmenterConfig {
    int sampleRate = 16000;
    int frameMs = 20;
    double minThresholdDb = -48.0;   // never treat anything quieter than this (RMS dBFS) as speech
    double marginDb = 9.0;           // speech must exceed noise floor by this much
    int minSpeechMs = 200;           // voiced run needed to open an utterance
    int hangoverMs = 700;            // silence that closes it
    int preRollMs = 240;             // audio kept from before the detected onset
    int softMaxMs = 8000;            // past this, cut at the first short pause (>= softCutSilenceMs)
    int softCutSilenceMs = 250;
    int maxUtteranceMs = 12000;      // hard cut
    int partialIntervalMs = 1500;    // 0 = no partials
    int minPartialMs = 800;
    int trailingKeepMs = 200;        // silence kept at the end of a final utterance
    int noiseWindowMs = 6000;        // if even the quietest frame of this window is above the floor, the floor rises to it
                                     // (steady noise / sustained tone is "noise"; real speech has pauses)
};

struct SegmentEvent {
    enum class Kind { Partial, Final };
    Kind kind = Kind::Partial;
    std::uint64_t id = 0;            // utterance id (same for its partials and its final)
    std::int64_t startMs = 0;        // on the session timeline
    std::int64_t endMs = 0;
    std::vector<float> audio;        // whole utterance so far
};

class Segmenter {
public:
    explicit Segmenter(SegmenterConfig cfg = {});
    std::vector<SegmentEvent> feed(const float* samples, std::size_t n);
    std::vector<SegmentEvent> flush();   // closes an open utterance (end of session)
    std::int64_t positionMs() const { return samplesToMs(totalSamples_); }
    bool inSpeech() const { return inSpeech_; }
    double noiseFloorDb() const { return noiseDb_; }

private:
    std::int64_t samplesToMs(std::uint64_t s) const { return static_cast<std::int64_t>(s * 1000 / static_cast<std::uint64_t>(cfg_.sampleRate)); }
    void processFrame(const float* f, std::vector<SegmentEvent>& out);
    void closeUtterance(std::vector<SegmentEvent>& out, bool endAtLastVoice);

    SegmenterConfig cfg_;
    std::size_t frameSamples_;
    std::vector<float> pending_;               // < 1 frame
    std::uint64_t totalSamples_ = 0;           // samples consumed as whole frames
    double noiseDb_;
    bool noiseInit_ = false;

    // idle state: recent frames (pre-roll + candidate run)
    std::deque<std::pair<std::uint64_t, double>> minWin_;   // monotonic deque (frame index, dB) for the window minimum
    std::uint64_t frameIdx_ = 0;
    std::deque<std::vector<float>> recent_;
    int voicedRun_ = 0;

    // speech state
    bool inSpeech_ = false;
    std::uint64_t utterId_ = 0;
    std::vector<float> utter_;
    std::int64_t utterStartMs_ = 0;
    std::int64_t lastVoiceEndMs_ = 0;
    std::size_t lastVoiceEndSample_ = 0;       // index into utter_ just after the last voiced frame
    int silenceMs_ = 0;
    std::int64_t lastPartialAtMs_ = 0;         // utterance length when the last partial went out
};

}  // namespace hb::captions
