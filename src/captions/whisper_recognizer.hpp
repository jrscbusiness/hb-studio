#pragma once
// Local, offline recogniser on whisper.cpp (MIT), CPU only. Built only with HB_CAPTIONS_WHISPER=ON.
// Whisper is not a streaming model: each call decodes one whole utterance (<= ~12 s, chosen by the
// segmenter). "Partial" results are produced by re-decoding the utterance so far.
#include <memory>
#include <string>

#include "captions/recognizer.hpp"

namespace hb::captions {

class WhisperRecognizer : public ISpeechRecognizer {
public:
    // threads <= 0: min(4, hardware_concurrency/2) (at least 1).
    WhisperRecognizer(std::string modelPathUtf8, int threads = 0);
    ~WhisperRecognizer() override;
    std::string name() const override { return "whisper.cpp"; }
    bool load(std::string& error) override;
    RecognizeResult recognize(const RecognizeRequest& request) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string modelPath_;
    int threads_;
};

}  // namespace hb::captions
