#include "ai/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "core/json.hpp"
#include "core/redact.hpp"

namespace hb::ai {

namespace {

using Json = hb::core::json::Value;

Json jstr(std::string s) {
    Json v;
    v.t = Json::T::Str;
    v.s = std::move(s);
    return v;
}
Json jnum(double n) {
    Json v;
    v.t = Json::T::Num;
    v.n = n;
    return v;
}
Json jbool(bool b) {
    Json v;
    v.t = Json::T::Bool;
    v.b = b;
    return v;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string sanitizeMsg(const std::string& in) {
    std::string red = hb::core::redactSecrets(in);
    std::string o;
    for (unsigned char c : red) {
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (o.size() >= 200) break;
        o += static_cast<char>(c);
    }
    return trim(o);
}

// Text of choices[0].message.content (string or array of {type:"text",text}).
std::string contentText(const Json& content) {
    if (content.isStr()) return content.s;
    if (content.isArr()) {
        std::string o;
        for (const auto& part : content.a) {
            if (!part.isObj()) continue;
            const Json* t = part.get("text");
            if (t && t->isStr()) o += t->s;
        }
        return o;
    }
    return {};
}

std::string errorFromObject(const Json& root) {
    if (!root.isObj()) return {};
    if (const Json* e = root.get("error")) {
        if (e->isStr()) return e->s;
        if (e->isObj()) {
            if (const Json* m = e->get("message"); m && m->isStr()) return m->s;
            return "erro sem mensagem";
        }
        return "erro sem mensagem";
    }
    return {};
}

}  // namespace

const char* kindId(ProviderKind k) {
    switch (k) {
        case ProviderKind::OpenRouter: return "openrouter";
        case ProviderKind::NineRouter: return "9router";
        default: return "custom";
    }
}
bool parseKind(const std::string& s, ProviderKind& out) {
    if (s == "openrouter") out = ProviderKind::OpenRouter;
    else if (s == "9router") out = ProviderKind::NineRouter;
    else if (s == "custom") out = ProviderKind::Custom;
    else return false;
    return true;
}
std::string defaultBaseUrl(ProviderKind k) {
    switch (k) {
        case ProviderKind::OpenRouter: return "https://openrouter.ai/api/v1";
        case ProviderKind::NineRouter: return "http://localhost:20128/v1";
        default: return "";
    }
}
std::string defaultName(ProviderKind k) {
    switch (k) {
        case ProviderKind::OpenRouter: return "OpenRouter";
        case ProviderKind::NineRouter: return "9router";
        default: return "Personalizado";
    }
}
const char* errorKindId(ErrorKind k) {
    switch (k) {
        case ErrorKind::None: return "none";
        case ErrorKind::Config: return "config";
        case ErrorKind::InvalidKey: return "invalid_key";
        case ErrorKind::NoCredit: return "no_credit";
        case ErrorKind::ModelUnavailable: return "model_unavailable";
        case ErrorKind::RateLimited: return "rate_limited";
        case ErrorKind::Network: return "network";
        case ErrorKind::Timeout: return "timeout";
        case ErrorKind::Server: return "server";
        case ErrorKind::BadResponse: return "bad_response";
        default: return "other";
    }
}

bool normalizeBaseUrl(const std::string& in, std::string& out, std::string& err) {
    std::string u = trim(in);
    for (unsigned char c : u)
        if (c < 0x21 || c == 0x7F) { err = "Endereço inválido: não pode ter espaços nem caracteres de controle."; return false; }
    std::string scheme;
    if (u.rfind("https://", 0) == 0) scheme = "https";
    else if (u.rfind("http://", 0) == 0) scheme = "http";
    else { err = "Endereço inválido: comece com https:// (ou http:// para localhost)."; return false; }
    std::string rest = u.substr(scheme.size() + 3);
    if (rest.find('?') != std::string::npos || rest.find('#') != std::string::npos) {
        err = "Endereço inválido: não use parâmetros (?) nem âncoras (#).";
        return false;
    }
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash);
    if (authority.empty()) { err = "Endereço inválido: falta o nome do servidor."; return false; }
    if (authority.find('@') != std::string::npos) {
        err = "Endereço inválido: não coloque usuário/senha no endereço; use o campo de chave.";
        return false;
    }
    // host[:port]; IPv6 literal in brackets.
    std::string host, port;
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) { err = "Endereço inválido (IPv6)."; return false; }
        host = authority.substr(1, close - 1);
        const std::string tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail[0] != ':') { err = "Endereço inválido (porta)."; return false; }
            port = tail.substr(1);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    if (host.empty()) { err = "Endereço inválido: falta o nome do servidor."; return false; }
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) ||
            std::atoi(port.c_str()) < 1 || std::atoi(port.c_str()) > 65535) {
            err = "Endereço inválido: porta fora do intervalo 1-65535.";
            return false;
        }
    }
    for (unsigned char c : host)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':')) { err = "Endereço inválido: nome de servidor com caracteres não permitidos."; return false; }
    if (scheme == "http") {
        const std::string h = lower(host);
        // Strict: "127.evil.com" must NOT pass as loopback (that would send the API key in clear to a remote host).
        bool dottedLoopback = false;
        {
            int octets = 0, value = 0, digits = 0, first = -1;
            bool ok = !h.empty();
            for (std::size_t i = 0; i <= h.size() && ok; ++i) {
                if (i == h.size() || h[i] == '.') {
                    if (digits == 0 || value > 255) { ok = false; break; }
                    if (octets == 0) first = value;
                    ++octets;
                    value = 0;
                    digits = 0;
                } else if (h[i] >= '0' && h[i] <= '9') {
                    value = value * 10 + (h[i] - '0');
                    if (++digits > 3) ok = false;
                } else {
                    ok = false;
                }
            }
            dottedLoopback = ok && octets == 4 && first == 127;
        }
        const bool loopback = h == "localhost" || h == "::1" || dottedLoopback;
        if (!loopback) {
            err = "HTTP sem criptografia só é permitido para localhost (9router, Ollama...). Use https:// para servidores remotos.";
            return false;
        }
    }
    while (!path.empty() && path.back() == '/') path.pop_back();
    out = scheme + "://" + authority + path;
    return true;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (path.empty() || path[0] == '/') return b + path;
    return b + "/" + path;
}

std::vector<std::pair<std::string, std::string>> buildHeaders(const Provider& p, bool sse) {
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Content-Type", "application/json");
    h.emplace_back("Accept", sse ? "text/event-stream" : "application/json");
    if (!p.apiKey.empty()) h.emplace_back("Authorization", "Bearer " + p.apiKey);
    if (p.kind == ProviderKind::OpenRouter) {
        // Optional attribution headers recommended by OpenRouter; contain nothing personal.
        h.emplace_back("X-Title", "HB Studio");
    }
    return h;
}

std::string buildChatBody(const std::string& model, const std::vector<ChatMessage>& msgs, const ChatOptions& opts, bool stream) {
    Json root;
    root.t = Json::T::Obj;
    root.o["model"] = jstr(model);
    Json arr;
    arr.t = Json::T::Arr;
    for (const auto& m : msgs) {
        Json mm;
        mm.t = Json::T::Obj;
        mm.o["role"] = jstr(m.role);
        mm.o["content"] = jstr(m.content);
        arr.a.push_back(std::move(mm));
    }
    root.o["messages"] = std::move(arr);
    root.o["temperature"] = jnum(std::clamp(opts.temperature, 0.0, 2.0));
    root.o["max_tokens"] = jnum(std::clamp(opts.maxTokens, 1, 32768));
    if (stream) root.o["stream"] = jbool(true);
    return hb::core::json::dump(root);
}

ParsedChat parseChatResponse(const std::string& body) {
    ParsedChat r;
    Json root;
    if (!hb::core::json::parse(body, root, hb::core::json::Limits{8 * 1024 * 1024, 16, 4096}) || !root.isObj()) return r;
    r.providerError = errorFromObject(root);
    if (!r.providerError.empty()) return r;
    const Json* choices = root.get("choices");
    if (!choices || !choices->isArr() || choices->a.empty() || !choices->a[0].isObj()) return r;
    const Json* msg = choices->a[0].get("message");
    if (!msg || !msg->isObj()) return r;
    const Json* content = msg->get("content");
    if (!content) return r;
    r.text = contentText(*content);
    r.ok = true;  // empty text is a valid (if useless) answer; callers decide
    return r;
}

void SseParser::feed(const std::string& chunk, std::vector<std::string>& deltas) {
    buf_ += chunk;
    if (buf_.size() > 4 * 1024 * 1024) buf_.clear();  // hostile endless line without newline
    std::size_t pos;
    while ((pos = buf_.find('\n')) != std::string::npos) {
        std::string l = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l, deltas);
    }
}

void SseParser::line(const std::string& l, std::vector<std::string>& deltas) {
    if (l.empty() || l[0] == ':') return;
    if (l.rfind("data:", 0) == 0) {
        const std::string payload = trim(l.substr(5));
        if (payload.empty()) return;
        ++events_;
        if (payload == "[DONE]") { done_ = true; return; }
        Json j;
        if (!hb::core::json::parse(payload, j) || !j.isObj()) return;
        if (std::string e = errorFromObject(j); !e.empty()) { error_ = e; return; }
        const Json* choices = j.get("choices");
        if (!choices || !choices->isArr() || choices->a.empty() || !choices->a[0].isObj()) return;
        const Json* delta = choices->a[0].get("delta");
        if (!delta || !delta->isObj()) return;
        const Json* c = delta->get("content");
        if (c) {
            std::string t = contentText(*c);
            if (!t.empty()) deltas.push_back(std::move(t));
        }
        return;
    }
    if (l.rfind("event:", 0) == 0 || l.rfind("id:", 0) == 0 || l.rfind("retry:", 0) == 0) return;
    if (plain_.size() < 1024 * 1024) plain_ += l + "\n";  // possibly a plain JSON body
}

void SseParser::finish(std::vector<std::string>& deltas) {
    if (!buf_.empty()) {
        std::string l = buf_;
        buf_.clear();
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l, deltas);
    }
    if (events_ == 0 && !plain_.empty()) {
        ParsedChat p = parseChatResponse(plain_);
        if (!p.providerError.empty()) error_ = p.providerError;
        else if (p.ok && !p.text.empty()) deltas.push_back(p.text);
        plain_.clear();
    }
}

std::string extractErrorMessage(const std::string& body) {
    Json root;
    if (!hb::core::json::parse(body, root, hb::core::json::Limits{1024 * 1024, 16, 4096}) || !root.isObj()) return {};
    std::string m = errorFromObject(root);
    if (m.empty()) {
        if (const Json* mm = root.get("message"); mm && mm->isStr()) m = mm->s;
    }
    return sanitizeMsg(m);
}

ErrorKind classifyHttpError(int status, const std::string& providerMsg) {
    if (status == 401 || status == 403) return ErrorKind::InvalidKey;
    if (status == 402) return ErrorKind::NoCredit;
    if (status == 404) return ErrorKind::ModelUnavailable;
    if (status == 429) return ErrorKind::RateLimited;
    if (status == 408) return ErrorKind::Timeout;
    if (status >= 500) return ErrorKind::Server;
    if (status == 400 || status == 422) {
        const std::string m = lower(providerMsg);
        if (m.find("model") != std::string::npos) return ErrorKind::ModelUnavailable;
    }
    return ErrorKind::Other;
}

bool isRetryable(int status) { return status == 429 || status == 408 || status >= 500; }

std::string errorMessagePt(ErrorKind kind, int status, const std::string& detail) {
    std::string m;
    switch (kind) {
        case ErrorKind::Config: m = "Configuração incompleta."; break;
        case ErrorKind::InvalidKey: m = "Chave de API inválida ou sem permissão. Confira se copiou a chave inteira e se ela é deste provedor."; break;
        case ErrorKind::NoCredit: m = "Sem créditos no provedor. Adicione créditos ou escolha um modelo grátis (':free')."; break;
        case ErrorKind::ModelUnavailable: m = "Modelo indisponível ou inexistente neste provedor. Atualize a lista de modelos e escolha outro."; break;
        case ErrorKind::RateLimited: m = "Limite de requisições atingido. Aguarde um pouco ou troque de modelo (modelos grátis têm limites baixos)."; break;
        case ErrorKind::Network: m = "Não foi possível conectar ao provedor. Verifique a internet e, no caso do 9router/Ollama, se o programa está rodando."; break;
        case ErrorKind::Timeout: m = "O provedor demorou demais para responder."; break;
        case ErrorKind::Server: m = "O provedor está com problemas (HTTP " + std::to_string(status) + "). Tente novamente em instantes."; break;
        case ErrorKind::BadResponse: m = "Resposta inesperada do provedor (não parece uma API compatível com OpenAI)."; break;
        case ErrorKind::None: return {};
        default: m = "Erro do provedor" + (status > 0 ? " (HTTP " + std::to_string(status) + ")" : std::string()) + "."; break;
    }
    if (!detail.empty()) m += " Detalhe: " + sanitizeMsg(detail);
    return m;
}

int parseRetryAfterSeconds(const HttpResponse& r) {
    for (const auto& h : r.headers) {
        if (lower(h.first) != "retry-after") continue;
        const std::string v = trim(h.second);
        if (v.empty() || v.size() > 6 || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return -1;
        return std::atoi(v.c_str());
    }
    return -1;
}

bool isFreeModel(const std::string& id, const std::string& promptPrice, const std::string& completionPrice, bool pricingKnown) {
    if (id.size() > 5 && id.compare(id.size() - 5, 5, ":free") == 0) return true;
    if (!pricingKnown || promptPrice.empty() || completionPrice.empty()) return false;
    auto zero = [](const std::string& s) {
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        return end != s.c_str() && *end == '\0' && d == 0.0;
    };
    return zero(promptPrice) && zero(completionPrice);
}

std::vector<ModelInfo> parseModels(const std::string& body) {
    std::vector<ModelInfo> out;
    Json root;
    if (!hb::core::json::parse(body, root, hb::core::json::Limits{8 * 1024 * 1024, 16, 8192}) || !root.isObj()) return out;
    const Json* data = root.get("data");
    if (!data || !data->isArr()) return out;
    for (const auto& m : data->a) {
        if (out.size() >= 2000) break;
        if (!m.isObj()) continue;
        const Json* id = m.get("id");
        if (!id || !id->isStr() || id->s.empty() || id->s.size() > 200) continue;
        ModelInfo mi;
        mi.id = id->s;
        if (const Json* n = m.get("name"); n && n->isStr()) mi.name = n->s.substr(0, 200);
        if (const Json* c = m.get("context_length"); c && c->isNum() && c->n > 0) mi.contextLength = static_cast<long long>(c->n);
        std::string pp, cp;
        bool known = false;
        if (const Json* pr = m.get("pricing"); pr && pr->isObj()) {
            const Json* a = pr->get("prompt");
            const Json* b = pr->get("completion");
            if (a && a->isStr() && b && b->isStr()) { pp = a->s; cp = b->s; known = true; }
        }
        mi.free = isFreeModel(mi.id, pp, cp, known);
        out.push_back(std::move(mi));
    }
    std::stable_sort(out.begin(), out.end(), [](const ModelInfo& a, const ModelInfo& b) {
        if (a.free != b.free) return a.free;
        return a.id < b.id;
    });
    return out;
}

std::string maskKey(const std::string& key) {
    if (key.empty()) return {};
    if (key.size() <= 8) return "••••";  // too short to reveal any part safely
    return "••••" + key.substr(key.size() - 4);
}

}  // namespace hb::ai
