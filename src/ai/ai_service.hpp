#pragma once
// Public contract for every AI-powered feature in HB Studio:
//
//   if (ai.configured()) {
//       auto r = ai.complete("You write short titles.", "Topic: Sunday service", {});
//       if (r.ok) use(r.text); else showToast(r.error);   // r.error is user-presentable pt-BR
//   }
//   ai.completeAsync(sys, user, opts, [](const hb::ai::ChatResult& r) { ... });  // runs on a worker thread
//
// It always uses the ACTIVE provider of the ProviderStore (CONFIG > Inteligência Artificial).
// Nothing is ever sent unless a feature calls complete(); there is no background traffic.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ai/ai_client.hpp"
#include "ai/provider_store.hpp"

namespace hb::ai {

struct ConnectionTest {
    bool ok = false;
    long long latencyMs = 0;
    std::string message;  // pt-BR, shown as-is
    ErrorKind kind = ErrorKind::None;
};

class AiService {
public:
    using Result = ChatResult;
    using Callback = std::function<void(const Result&)>;

    // `store` must outlive the service. The destructor waits for running async jobs (bounded by
    // the per-request timeout, 30 s by default).
    AiService(ProviderStore& store, AiClient client);
    ~AiService();
    AiService(const AiService&) = delete;
    AiService& operator=(const AiService&) = delete;

    // True when an active provider has a model and (if it requires one) a key. Cheap, no I/O.
    bool configured() const;

    // Blocking. Never throws. Not configured -> ok=false with a pt-BR explanation.
    Result complete(const std::string& systemPrompt, const std::string& userPrompt, const ChatOptions& opts = {}) const;

    // Runs complete() on a worker thread and calls `cb` there (cb must be thread-safe).
    void completeAsync(std::string systemPrompt, std::string userPrompt, ChatOptions opts, Callback cb);

    // Used by the CONFIG card; `p` may be an unsaved draft. Sends a 1-token chat when a model is set
    // (validates key AND model), otherwise only lists models.
    ConnectionTest testConnection(const Provider& p) const;
    ModelsResult listModels(const Provider& p) const;

    // Generic worker: runs `job` on a tracked thread (joined in the destructor).
    void runAsync(std::function<void()> job);

private:
    struct Worker {
        std::shared_ptr<std::atomic<bool>> done;
        std::thread th;
    };
    void reap();  // joins finished workers; requires mutex_

    ProviderStore& store_;
    AiClient client_;
    mutable std::mutex mutex_;
    std::vector<Worker> workers_;
    bool closing_ = false;
};

}  // namespace hb::ai
