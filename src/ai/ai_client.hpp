#pragma once
// OpenAI-compatible chat/completions + models client with retry/backoff. Stateless apart from its
// injected dependencies; safe to call from several threads if the transport is.
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "ai/http.hpp"
#include "ai/types.hpp"

namespace hb::ai {

struct AiClientDeps {
    std::shared_ptr<IAiHttp> http;
    // Injectable for deterministic tests; defaults: steady_clock::now / std::this_thread::sleep_for.
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
};

class AiClient {
public:
    explicit AiClient(AiClientDeps deps);

    // Validates the provider (base URL, model, key when required), then calls POST {base}/chat/completions.
    // Retries 429/408/5xx and network errors up to opts.maxRetries with exponential backoff
    // (500ms, 1s, 2s... capped at 8s; honours Retry-After up to 8s). A streaming call is never
    // retried once a fragment has been delivered.
    ChatResult chat(const Provider& p, const std::vector<ChatMessage>& msgs, const ChatOptions& opts) const;

    // GET {base}/models. The API key is sent when present (some routers require it).
    ModelsResult listModels(const Provider& p, std::chrono::milliseconds timeout = std::chrono::milliseconds(20000)) const;

    static constexpr std::chrono::milliseconds kBackoffCap{8000};

private:
    AiClientDeps d_;
};

}  // namespace hb::ai
