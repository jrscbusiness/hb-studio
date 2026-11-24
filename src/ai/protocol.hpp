#pragma once
// Pure (no I/O) request builders and response parsers for OpenAI-compatible APIs.
#include <string>
#include <utility>
#include <vector>

#include "ai/http.hpp"
#include "ai/types.hpp"

namespace hb::ai {

// Validates and normalises a base URL: http(s)://host[:port][/path], no credentials, no query.
// http:// is accepted only for loopback hosts (localhost, 127.x, ::1) so keys never travel in clear
// text over a network. On failure returns false and sets `err` (pt-BR).
bool normalizeBaseUrl(const std::string& in, std::string& out, std::string& err);
std::string joinUrl(const std::string& base, const std::string& path);  // "a/v1" + "/chat/completions"

std::vector<std::pair<std::string, std::string>> buildHeaders(const Provider& p, bool sse);
std::string buildChatBody(const std::string& model, const std::vector<ChatMessage>& msgs, const ChatOptions& opts, bool stream);

struct ParsedChat {
    bool ok = false;
    std::string text;
    std::string providerError;  // message from an {"error":{...}} object, if any
};
ParsedChat parseChatResponse(const std::string& body);

// Incremental Server-Sent-Events parser for chat/completions streams. Feed raw chunks; collects
// text deltas. Tolerates CRLF, comment lines (": OPENROUTER PROCESSING") and chunk boundaries
// inside a line. If the server ignored stream=true and answered with plain JSON, finish() parses it.
class SseParser {
public:
    void feed(const std::string& chunk, std::vector<std::string>& deltas);  // appends new fragments
    void finish(std::vector<std::string>& deltas);  // once at the end; handles a non-SSE JSON body
    bool done() const { return done_; }
    const std::string& providerError() const { return error_; }
    bool sawEvents() const { return events_ > 0; }

private:
    void line(const std::string& l, std::vector<std::string>& deltas);
    std::string buf_, plain_, error_;
    int events_ = 0;
    bool done_ = false;
};

// {"error":{"message":"..."}} / {"error":"..."} / {"message":"..."} -> message (sanitised, <=200 chars).
std::string extractErrorMessage(const std::string& body);
ErrorKind classifyHttpError(int status, const std::string& providerMsg);
bool isRetryable(int status);
std::string errorMessagePt(ErrorKind kind, int status, const std::string& providerMsg);
int parseRetryAfterSeconds(const HttpResponse& r);  // -1 when absent/invalid

// Free when the id ends in ":free", or (pricing known) both prompt and completion prices are 0.
// OpenRouter uses "-1" for dynamic routers: never free.
bool isFreeModel(const std::string& id, const std::string& promptPrice, const std::string& completionPrice, bool pricingKnown);
// Parses OpenRouter ({"data":[{id,name,pricing:{prompt,completion},context_length}]}) or the plain
// OpenAI/9router/Ollama shape ({"data":[{"id":..}]}). Free models first, then by id; max 2000.
std::vector<ModelInfo> parseModels(const std::string& body);

std::string maskKey(const std::string& key);  // "" or a bullet run + last 4 chars

}  // namespace hb::ai
