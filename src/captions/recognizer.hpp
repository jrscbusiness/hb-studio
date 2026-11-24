#pragma once
// Speech-recognition abstraction for "Legendas ao vivo". Input is 16 kHz mono float PCM (one
// utterance at a time, chosen by the segmenter); output is plain text. Implementations:
//   * WhisperRecognizer  (whisper.cpp, local/offline)   - whisper_recognizer.cpp, optional build
//   * CloudRecognizer    (OpenAI-compatible /audio/transcriptions) - cloud_recognizer.cpp
//   * FakeRecognizer     (tests)
// recognize() is only ever called from the single caption worker thread, never from the audio
// engine thread, so implementations may block (inference) but must not throw past this boundary.
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hb::captions {

constexpr int kRecognizerSampleRate = 16000;

struct RecognizeRequest {
    const float* pcm = nullptr;   // mono, 16 kHz, nominally -1..1
    std::size_t samples = 0;
    std::string language = "pt";  // ISO-639-1 or "auto"
    bool partial = false;         // true: interim hypothesis of an utterance still in progress
};

struct RecognizeResult {
    bool ok = false;
    std::string text;   // UTF-8, un-normalised
    std::string error;  // set when !ok (never contains secrets)
};

class ISpeechRecognizer {
public:
    virtual ~ISpeechRecognizer() = default;
    virtual std::string name() const = 0;
    // Engines that cost money or time per call (cloud) return false: only finals are requested.
    virtual bool supportsPartials() const { return true; }
    // Loads models / validates configuration. May take seconds; called on the worker thread.
    virtual bool load(std::string& error) { (void)error; return true; }
    virtual RecognizeResult recognize(const RecognizeRequest& request) = 0;
};

// Scriptable recognizer for tests. The callback receives the request and the 0-based call index.
class FakeRecognizer : public ISpeechRecognizer {
public:
    using Script = std::function<std::string(const RecognizeRequest&, int call)>;
    explicit FakeRecognizer(Script script, bool partials = true) : script_(std::move(script)), partials_(partials) {}
    std::string name() const override { return "fake"; }
    bool supportsPartials() const override { return partials_; }
    bool load(std::string& error) override {
        if (!loadError.empty()) { error = loadError; return false; }
        return true;
    }
    RecognizeResult recognize(const RecognizeRequest& r) override {
        const int call = calls_++;
        lastSamples = r.samples;
        lastWasPartial = r.partial;
        RecognizeResult out;
        if (failAll) { out.error = "falha simulada"; return out; }
        out.ok = true;
        out.text = script_ ? script_(r, call) : std::string();
        return out;
    }
    int calls() const { return calls_; }
    std::string loadError;
    bool failAll = false;
    std::size_t lastSamples = 0;
    bool lastWasPartial = false;

private:
    Script script_;
    bool partials_;
    int calls_ = 0;
};

}  // namespace hb::captions
