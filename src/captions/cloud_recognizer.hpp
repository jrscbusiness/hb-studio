#pragma once
// OPTIONAL cloud engine: any server that implements the OpenAI-style
//   POST {baseUrl}/audio/transcriptions   (multipart: file=<wav>, model, language, response_format=json)
// e.g. OpenAI itself, Groq, or a self-hosted faster-whisper/whisper.cpp server. NOTE: many API
// aggregators (OpenRouter among them) do NOT offer speech-to-text at all - this is the user's
// responsibility to verify. Using this engine SENDS THE PROGRAM AUDIO to that server (see LEGENDAS.md).
// Only final utterances are sent (no partials) to keep request count and cost down.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "captions/recognizer.hpp"

namespace hb::captions {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;   // transport error text ("" when a response arrived)
};

class IHttpPoster {
public:
    virtual ~IHttpPoster() = default;
    virtual HttpResponse post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                              const std::string& body, int timeoutMs) = 0;
};

struct CloudConfig {
    std::string baseUrl;             // e.g. https://api.openai.com/v1
    std::string apiKey;
    std::string model = "whisper-1";
    int timeoutMs = 20000;
};

// 16-bit PCM mono WAV (RIFF) of float samples (clamped).
std::string encodeWav16(const float* pcm, std::size_t samples, int sampleRate);
// Multipart/form-data body; boundary must not occur in the parts (callers use a random-looking one).
std::string buildMultipart(const std::string& boundary, const std::vector<std::pair<std::string, std::string>>& fields,
                           const std::string& fileField, const std::string& filename, const std::string& fileContentType,
                           const std::string& fileData);
// Pulls "text" out of {"text":"..."}; false on any other shape.
bool parseTranscriptionJson(const std::string& body, std::string& text);
// https only, except plain http to loopback (local servers). Returns "" if acceptable, else the reason.
std::string validateBaseUrl(const std::string& url);
// Joins baseUrl + "/audio/transcriptions" without doubling slashes.
std::string transcriptionsUrl(const std::string& baseUrl);

class CloudRecognizer : public ISpeechRecognizer {
public:
    CloudRecognizer(CloudConfig cfg, IHttpPoster* http) : cfg_(std::move(cfg)), http_(http) {}
    std::string name() const override { return "cloud"; }
    bool supportsPartials() const override { return false; }
    bool load(std::string& error) override;
    RecognizeResult recognize(const RecognizeRequest& request) override;

private:
    CloudConfig cfg_;
    IHttpPoster* http_;
    std::uint64_t counter_ = 0;
};

}  // namespace hb::captions
