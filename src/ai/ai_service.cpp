#include "ai/ai_service.hpp"

#include <stdexcept>
#include <utility>

namespace hb::ai {

AiService::AiService(ProviderStore& store, AiClient client) : store_(store), client_(std::move(client)) {}

AiService::~AiService() {
    std::vector<Worker> ws;
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        ws = std::move(workers_);
    }
    for (auto& w : ws)
        if (w.th.joinable()) w.th.join();
}

bool AiService::configured() const {
    const Provider p = store_.active();
    if (p.empty() || p.model.empty()) return false;
    return !p.keyRequired() || !p.apiKey.empty();
}

AiService::Result AiService::complete(const std::string& systemPrompt, const std::string& userPrompt, const ChatOptions& opts) const {
    try {
        const Provider p = store_.active();
        if (p.empty()) {
            Result r;
            r.kind = ErrorKind::Config;
            r.error = "Nenhum provedor de IA configurado. Abra CONFIG > Inteligência Artificial.";
            return r;
        }
        std::vector<ChatMessage> msgs;
        if (!systemPrompt.empty()) msgs.push_back({"system", systemPrompt});
        msgs.push_back({"user", userPrompt});
        return client_.chat(p, msgs, opts);
    } catch (const std::exception& e) {
        Result r;
        r.kind = ErrorKind::Other;
        r.error = std::string("Erro inesperado na IA: ") + e.what();
        return r;
    } catch (...) {
        Result r;
        r.kind = ErrorKind::Other;
        r.error = "Erro inesperado na IA.";
        return r;
    }
}

void AiService::reap() {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load()) {
            if (it->th.joinable()) it->th.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void AiService::runAsync(std::function<void()> job) {
    std::lock_guard lock(mutex_);
    if (closing_) return;
    reap();
    auto done = std::make_shared<std::atomic<bool>>(false);
    Worker w;
    w.done = done;
    w.th = std::thread([job = std::move(job), done] {
        try { job(); } catch (...) {}
        done->store(true);
    });
    workers_.push_back(std::move(w));
}

void AiService::completeAsync(std::string systemPrompt, std::string userPrompt, ChatOptions opts, Callback cb) {
    runAsync([this, s = std::move(systemPrompt), u = std::move(userPrompt), o = std::move(opts), cb = std::move(cb)] {
        Result r = complete(s, u, o);
        if (cb) cb(r);
    });
}

ConnectionTest AiService::testConnection(const Provider& p) const {
    ConnectionTest t;
    try {
        if (!p.model.empty()) {
            ChatOptions o;
            o.maxTokens = 1;
            o.maxRetries = 0;
            o.timeout = std::chrono::milliseconds(20000);
            Result r = client_.chat(p, {{"user", "ping"}}, o);
            t.ok = r.ok;
            t.kind = r.kind;
            t.latencyMs = r.latencyMs;
            t.message = r.ok ? "Conexão OK: o modelo respondeu." : r.error;
        } else {
            ModelsResult m = client_.listModels(p);
            t.ok = m.ok;
            t.kind = m.kind;
            t.latencyMs = m.latencyMs;
            t.message = m.ok ? "Conexão OK (" + std::to_string(m.models.size()) +
                                   " modelos disponíveis). Escolha um modelo para validar também a chave."
                             : m.error;
        }
    } catch (...) {
        t.ok = false;
        t.kind = ErrorKind::Other;
        t.message = "Erro inesperado ao testar a conexão.";
    }
    return t;
}

ModelsResult AiService::listModels(const Provider& p) const {
    try {
        return client_.listModels(p);
    } catch (...) {
        ModelsResult m;
        m.kind = ErrorKind::Other;
        m.error = "Erro inesperado ao listar modelos.";
        return m;
    }
}

}  // namespace hb::ai
