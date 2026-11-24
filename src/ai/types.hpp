#pragma once
// Shared value types for the AI module (providers, chat messages, results).
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace hb::ai {

enum class ProviderKind { OpenRouter, NineRouter, Custom };

const char* kindId(ProviderKind k);                       // "openrouter" | "9router" | "custom"
bool parseKind(const std::string& s, ProviderKind& out);  // false for unknown ids
std::string defaultBaseUrl(ProviderKind k);               // "" for Custom
std::string defaultName(ProviderKind k);                  // display name

// One saved AI provider. `apiKey` is persisted encrypted (ProviderStore) and NEVER sent to the UI
// (only a masked hint).
struct Provider {
    std::string id;       // assigned by the store ("p1", "p2", ...)
    std::string name;
    ProviderKind kind = ProviderKind::Custom;
    std::string baseUrl;  // normalised: no trailing slash, e.g. https://openrouter.ai/api/v1
    std::string apiKey;
    std::string model;    // default model id for this provider

    // OpenRouter always needs a key; 9router (local) and Custom (Ollama/LM Studio) may not.
    bool keyRequired() const { return kind == ProviderKind::OpenRouter; }
    bool empty() const { return id.empty(); }
};

struct ChatMessage {
    std::string role;  // "system" | "user" | "assistant"
    std::string content;
};

struct ChatOptions {
    double temperature = 0.7;
    int maxTokens = 512;
    std::chrono::milliseconds timeout{30000};  // per HTTP attempt
    int maxRetries = 2;                        // extra attempts on 429/5xx/network; 0 = none
    // If set, the request streams (SSE) and each text fragment is delivered here as it arrives.
    // The final ChatResult::text still holds the full text. Called on the calling thread.
    std::function<void(const std::string&)> onDelta;
};

enum class ErrorKind { None, Config, InvalidKey, NoCredit, ModelUnavailable, RateLimited, Network, Timeout, Server, BadResponse, Other };
const char* errorKindId(ErrorKind k);

struct ChatResult {
    bool ok = false;
    std::string text;
    std::string error;  // pt-BR, user-presentable; empty when ok
    ErrorKind kind = ErrorKind::None;
    int httpStatus = 0;
    int attempts = 0;
    long long latencyMs = 0;
};

struct ModelInfo {
    std::string id;
    std::string name;
    bool free = false;
    long long contextLength = 0;
};

struct ModelsResult {
    bool ok = false;
    std::vector<ModelInfo> models;
    std::string error;
    ErrorKind kind = ErrorKind::None;
    long long latencyMs = 0;
};

}  // namespace hb::ai
