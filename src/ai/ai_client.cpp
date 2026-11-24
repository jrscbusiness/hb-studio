#include "ai/ai_client.hpp"

#include <algorithm>
#include <thread>

#include "ai/protocol.hpp"

namespace hb::ai {

namespace {

constexpr std::size_t kMaxStreamText = 256 * 1024;

std::string validate(const Provider& p, bool needModel, std::string& baseOut) {
    std::string err;
    if (!normalizeBaseUrl(p.baseUrl, baseOut, err)) return err;
    if (needModel && p.model.empty()) return "Escolha um modelo para este provedor.";
    if (p.keyRequired() && p.apiKey.empty()) return "Este provedor exige uma chave de API. Cole sua chave e salve.";
    return {};
}

ChatResult fail(ErrorKind k, int status, const std::string& detail) {
    ChatResult r;
    r.kind = k;
    r.httpStatus = status;
    r.error = errorMessagePt(k, status, detail);
    return r;
}

}  // namespace

AiClient::AiClient(AiClientDeps deps) : d_(std::move(deps)) {
    if (!d_.now) d_.now = [] { return std::chrono::steady_clock::now(); };
    if (!d_.sleep) d_.sleep = [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); };
}

ChatResult AiClient::chat(const Provider& p, const std::vector<ChatMessage>& msgs, const ChatOptions& opts) const {
    std::string base;
    if (std::string e = validate(p, true, base); !e.empty()) {
        ChatResult r;
        r.kind = ErrorKind::Config;
        r.error = e;
        return r;
    }
    if (!d_.http) return fail(ErrorKind::Network, 0, "transporte HTTP indisponível");

    const bool stream = static_cast<bool>(opts.onDelta);
    HttpRequest req;
    req.method = "POST";
    req.url = joinUrl(base, "/chat/completions");
    req.headers = buildHeaders(p, stream);
    req.body = buildChatBody(p.model, msgs, opts, stream);
    req.timeout = opts.timeout;

    const auto t0 = d_.now();
    const int maxAttempts = 1 + std::clamp(opts.maxRetries, 0, 5);
    ChatResult last;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        SseParser sse;
        std::string streamed;
        bool delivered = false;
        bool truncated = false;
        if (stream) {
            req.onData = [&](const std::string& chunk) {
                std::vector<std::string> ds;
                sse.feed(chunk, ds);
                for (auto& s : ds) {
                    if (streamed.size() + s.size() > kMaxStreamText) { truncated = true; return false; }
                    streamed += s;
                    delivered = true;
                    try { opts.onDelta(s); } catch (...) {}  // a throwing UI callback must not kill the stream
                }
                return true;
            };
        }
        HttpResponse resp = d_.http->send(req);
        const int status = resp.status;
        int retryAfter = parseRetryAfterSeconds(resp);

        ChatResult r;
        bool retry = false;
        if (!resp.transportError.empty()) {
            r = fail(resp.timedOut ? ErrorKind::Timeout : ErrorKind::Network, 0, resp.timedOut ? "" : resp.transportError);
            retry = !delivered;
        } else if (status >= 200 && status < 300) {
            if (stream) {
                std::vector<std::string> ds;
                sse.finish(ds);
                for (auto& s : ds) {
                    streamed += s;
                    delivered = true;
                    try { opts.onDelta(s); } catch (...) {}
                }
                if (!sse.providerError().empty() && streamed.empty()) {
                    r = fail(ErrorKind::Other, status, sse.providerError());
                    retry = false;
                } else if (streamed.empty() && !truncated) {
                    r = fail(ErrorKind::BadResponse, status, "resposta vazia");
                } else {
                    r.ok = true;
                    r.text = streamed;
                }
            } else {
                ParsedChat pc = parseChatResponse(resp.body);
                if (pc.ok) {
                    r.ok = true;
                    r.text = pc.text;
                } else if (!pc.providerError.empty()) {
                    // HTTP 200 carrying an error object (seen with OpenRouter upstream failures).
                    const ErrorKind k = classifyHttpError(0, pc.providerError);
                    r = fail(k == ErrorKind::Other ? ErrorKind::Other : k, status, pc.providerError);
                } else {
                    r = fail(ErrorKind::BadResponse, status, "");
                }
            }
        } else {
            const std::string msg = extractErrorMessage(resp.body);
            r = fail(classifyHttpError(status, msg), status, msg);
            retry = isRetryable(status) && !delivered;
        }
        r.httpStatus = status;
        r.attempts = attempt + 1;
        r.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(d_.now() - t0).count();
        last = r;
        if (r.ok || !retry || attempt + 1 >= maxAttempts) break;

        std::chrono::milliseconds delay{500LL << std::min(attempt, 4)};
        if (retryAfter >= 0) delay = std::chrono::milliseconds(static_cast<long long>(retryAfter) * 1000);
        delay = std::min(delay, kBackoffCap);
        d_.sleep(delay);
    }
    return last;
}

ModelsResult AiClient::listModels(const Provider& p, std::chrono::milliseconds timeout) const {
    ModelsResult out;
    std::string base;
    if (std::string e = validate(p, false, base); !e.empty()) {
        out.kind = ErrorKind::Config;
        out.error = e;
        return out;
    }
    if (!d_.http) {
        out.kind = ErrorKind::Network;
        out.error = errorMessagePt(ErrorKind::Network, 0, "transporte HTTP indisponível");
        return out;
    }
    HttpRequest req;
    req.method = "GET";
    req.url = joinUrl(base, "/models");
    req.headers = buildHeaders(p, false);
    req.timeout = timeout;
    const auto t0 = d_.now();
    HttpResponse resp = d_.http->send(req);
    out.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(d_.now() - t0).count();
    if (!resp.transportError.empty()) {
        out.kind = resp.timedOut ? ErrorKind::Timeout : ErrorKind::Network;
        out.error = errorMessagePt(out.kind, 0, resp.timedOut ? "" : resp.transportError);
        return out;
    }
    if (resp.status < 200 || resp.status >= 300) {
        const std::string msg = extractErrorMessage(resp.body);
        out.kind = classifyHttpError(resp.status, msg);
        // A 404 on /models means "no such endpoint", not "model unavailable".
        if (resp.status == 404) {
            out.error = "Este endereço não tem a lista de modelos (/models). Confira o endereço base; digite o nome do modelo manualmente.";
            out.kind = ErrorKind::BadResponse;
        } else {
            out.error = errorMessagePt(out.kind, resp.status, msg);
        }
        return out;
    }
    out.models = parseModels(resp.body);
    if (out.models.empty()) {
        out.kind = ErrorKind::BadResponse;
        out.error = "O provedor não devolveu nenhum modelo (ou a resposta não é compatível com OpenAI).";
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace hb::ai
