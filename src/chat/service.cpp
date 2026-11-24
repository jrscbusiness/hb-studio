#include "chat/service.hpp"

#include <algorithm>
#include <sstream>

#include "core/json.hpp"

namespace hb::chat {
namespace {

using Json = hb::core::json::Value;
std::string esc(const std::string& s) { return hb::core::json::escape(s); }

constexpr const char* kYtSecretKey = "youtube.clientSecret";
constexpr const char* kFbTokenKey = "facebook.token";

class NullHttp : public IChatHttp {
public:
    HttpResponse send(const HttpRequest&) override {
        HttpResponse r;
        r.error = "rede indisponível nesta plataforma";
        return r;
    }
};
class NullSocket : public IChatSocket {
public:
    bool connect(const std::string&, std::string& e) override { e = "rede indisponível nesta plataforma"; return false; }
    bool sendText(const std::string&) override { return false; }
    Recv receive(std::string&, int) override { return Recv::Closed; }
    void close() override {}
};

const char* kAuthHtml =
    "<!doctype html><html lang=\"pt-BR\"><meta charset=\"utf-8\"><title>HB Studio</title>"
    "<body style=\"font-family:Segoe UI,sans-serif;text-align:center;margin-top:18vh\">"
    "<h2>HB Studio</h2><p>Autoriza\xC3\xA7\xC3\xA3o recebida. Voc\xC3\xAA j\xC3\xA1 pode fechar esta aba e voltar ao aplicativo.</p></body></html>";

std::vector<std::string> splitCmd(const std::string& msg, std::size_t maxParts) {
    std::vector<std::string> a;
    std::size_t s0 = 0;
    for (std::size_t i = 0; i <= msg.size(); ++i)
        if (i == msg.size() || (msg[i] == '|' && a.size() + 1 < maxParts)) { a.push_back(msg.substr(s0, i - s0)); s0 = i + 1; }
    return a;
}

bool parseSeq(const std::string& s, std::uint64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return true;
}

std::string strField(const Json& o, const char* k) {
    const Json* v = o.get(k);
    return v && v->isStr() ? v->s : std::string();
}

}  // namespace

ChatService::ChatService(ChatDeps deps)
    : deps_(std::move(deps)),
      secrets_(deps_.secrets ? deps_.secrets : static_cast<ISecretStore*>(&memSecrets_)),
      hub_(deps_.bus),
      nullHttp_(deps_.http ? nullptr : std::make_unique<NullHttp>()),
      nullSocket_(deps_.socket ? nullptr : std::make_unique<NullSocket>()),
      tokens_(deps_.http ? *deps_.http : *nullHttp_, *secrets_),
      yt_(deps_.http ? *deps_.http : *nullHttp_, tokens_, deps_.epochMs),
      fb_(deps_.http ? *deps_.http : *nullHttp_,
          [this] {
              std::string t;
              secrets_->get(kFbTokenKey, t);
              return t;
          },
          deps_.epochMs),
      ig_(deps_.epochMs),
      tw_(deps_.socket ? *deps_.socket : *nullSocket_, deps_.epochMs) {
    for (auto& w : wanted_) w = false;
    if (!deps_.settingsPath.empty()) settings_.load(deps_.settingsPath);
    hub_.setMuted(settings_.muted);
    applySettings();
}

ChatService::~ChatService() { stop(); }

IChatSource* ChatService::source(Platform p) {
    switch (p) {
        case Platform::YouTube: return &yt_;
        case Platform::Facebook: return &fb_;
        case Platform::Instagram: return &ig_;
        case Platform::Twitch: return &tw_;
    }
    return nullptr;
}

void ChatService::applySettings() {
    ChatSettings s;
    {
        std::lock_guard lock(m_);
        s = settings_;
    }
    std::string sec;
    secrets_->get(kYtSecretKey, sec);
    tokens_.setClient(s.youtubeClientId, sec);
    YouTubeConfig y;
    y.videoId = s.youtubeVideoId;
    yt_.configure(y);
    FacebookConfig f;
    f.liveVideoId = s.facebookLiveVideoId;
    fb_.configure(f);
    TwitchConfig t;
    t.channel = s.twitchChannel;
    tw_.configure(t);
    hub_.setLog(s.persistMessages && !deps_.logPath.empty() ? std::make_unique<FileMessageLog>(deps_.logPath) : nullptr);
}

void ChatService::persistSettings() {
    ChatSettings s;
    {
        std::lock_guard lock(m_);
        settings_.muted = hub_.muted();
        s = settings_;
    }
    if (!deps_.settingsPath.empty() && !s.save(deps_.settingsPath)) say("Não foi possível salvar as configurações do chat.");
}

bool ChatService::saveSecret(const char* key, const std::string& v) {
    if (v.empty()) return true;   // blank = keep what is stored
    if (!secrets_->put(key, v)) { say("Não foi possível guardar o segredo com segurança."); return false; }
    return true;
}

std::string ChatService::secretHint(const char* key) const {
    std::string v;
    return secrets_->get(key, v) ? maskTail(v) : std::string();
}

void ChatService::stepSource(int idx, TimePoint now) {
    IChatSource* src = source(static_cast<Platform>(idx));
    const bool want = wanted_[idx];
    try {
        if (want && !connected_[idx]) { src->connect(); connected_[idx] = true; }
        else if (!want && connected_[idx]) { src->disconnect(); connected_[idx] = false; }
        if (connected_[idx]) {
            auto msgs = src->poll(now);
            if (!msgs.empty()) hub_.ingest(std::move(msgs));
        }
    } catch (...) {
        // an adapter bug must never take the app down; the source keeps its last status
    }
}

void ChatService::pumpOnce(TimePoint now) {
    for (int i = 0; i < kPlatformCount; ++i) stepSource(i, now);
}

void ChatService::runnerLoop(int idx) {
    while (!quit_) {
        stepSource(idx, std::chrono::steady_clock::now());
        std::unique_lock lock(cvm_);
        cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return quit_.load(); });
    }
    if (connected_[idx]) {
        try { source(static_cast<Platform>(idx))->disconnect(); } catch (...) {}
        connected_[idx] = false;
    }
}

void ChatService::flusherLoop() {
    while (!quit_) {
        {
            std::unique_lock lock(cvm_);
            cv_.wait_for(lock, std::chrono::milliseconds(std::max(50, deps_.flushIntervalMs)), [&] { return quit_.load(); });
        }
        if (quit_) break;
        if (!deps_.post) continue;
        const std::string batch = hub_.takeBatchJson(50);
        if (!batch.empty()) deps_.post("", batch);   // empty key: batches are never coalesced
        const std::string st = statusJson();
        if (st != lastStatusSent_) { lastStatusSent_ = st; deps_.post("chat_status", st); }
    }
}

void ChatService::start() {
    if (started_.exchange(true)) return;
    for (int i = 0; i < kPlatformCount; ++i) threads_.emplace_back([this, i] { runnerLoop(i); });
    threads_.emplace_back([this] { flusherLoop(); });
}

void ChatService::stop() {
    if (!started_.exchange(false)) {
        // never started (tests) - still make sure the auth thread is gone
    }
    quit_ = true;
    authCancel_ = true;
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    if (authThread_.joinable()) authThread_.join();
}

void ChatService::setWanted(Platform p, bool want) {
    wanted_[static_cast<int>(p)] = want;
    cv_.notify_all();
}

int ChatService::ingestInstagramWebhook(const std::string& body) { return ig_.feedWebhook(body); }

std::string ChatService::settingsJson() const {
    ChatSettings s;
    std::string authState, authMsg;
    {
        std::lock_guard lock(m_);
        s = settings_;
    }
    std::ostringstream o;
    o << "{\"t\":\"chat_settings\",\"youtube\":{\"videoId\":\"" << esc(s.youtubeVideoId) << "\",\"clientId\":\""
      << esc(s.youtubeClientId) << "\",\"hasSecret\":" << (secretHint(kYtSecretKey).empty() ? "false" : "true")
      << ",\"secretHint\":\"" << esc(secretHint(kYtSecretKey)) << "\",\"authorized\":"
      << (const_cast<GoogleTokenManager&>(tokens_).hasRefreshToken() ? "true" : "false")
      << "},\"facebook\":{\"liveVideoId\":\"" << esc(s.facebookLiveVideoId) << "\",\"hasToken\":"
      << (secretHint(kFbTokenKey).empty() ? "false" : "true") << ",\"tokenHint\":\"" << esc(secretHint(kFbTokenKey))
      << "\"},\"twitch\":{\"channel\":\"" << esc(s.twitchChannel) << "\"},\"persist\":"
      << (s.persistMessages ? "true" : "false") << ",\"net\":" << (deps_.http ? "true" : "false") << "}";
    return o.str();
}

std::string ChatService::statusJson() const {
    std::string authState, authMsg;
    {
        std::lock_guard lock(m_);
        authState = authState_;
        authMsg = authMessage_;
    }
    std::ostringstream o;
    o << "{\"t\":\"chat_status\",\"platforms\":[";
    for (int i = 0; i < kPlatformCount; ++i) {
        const auto p = static_cast<Platform>(i);
        const SourceStatus st = const_cast<ChatService*>(this)->source(p)->status();
        o << (i ? "," : "") << "{\"p\":\"" << platformKey(p) << "\",\"state\":\"" << sourceStateName(st.state)
          << "\",\"detail\":\"" << esc(st.detail) << "\",\"error\":\"" << esc(st.lastError) << "\",\"quota\":"
          << st.quotaUnits << ",\"want\":" << (wanted_[i] ? "true" : "false") << "}";
    }
    o << "],\"auth\":{\"state\":\"" << esc(authState) << "\",\"message\":\"" << esc(authMsg) << "\"},\"muted\":[";
    const auto muted = hub_.muted();
    for (std::size_t i = 0; i < muted.size(); ++i) o << (i ? "," : "") << "\"" << esc(muted[i]) << "\"";
    o << "],\"total\":" << hub_.totalAccepted() << "}";
    return o.str();
}

void ChatService::postAll() {
    if (!deps_.post) return;
    deps_.post("chat_settings", settingsJson());
    lastStatusSent_ = statusJson();
    deps_.post("chat_status", lastStatusSent_);
    deps_.post("", hub_.historyJson(200));
}

void ChatService::startAuth() {
    if (authRunning_.exchange(true)) { say("A autorização já está em andamento."); return; }
    if (authThread_.joinable()) authThread_.join();
    ChatSettings s;
    {
        std::lock_guard lock(m_);
        s = settings_;
    }
    if (s.youtubeClientId.empty()) {
        authRunning_ = false;
        say("Informe e salve o Client ID do Google antes de autorizar.");
        return;
    }
    authCancel_ = false;
    { std::lock_guard lock(m_); authState_ = "waiting"; authMessage_ = "Abrindo o navegador..."; }
    authThread_ = std::thread([this] { authThreadMain(); });
}

void ChatService::authThreadMain() {
    auto finish = [&](const std::string& state, const std::string& msg) {
        { std::lock_guard lock(m_); authState_ = state; authMessage_ = msg; }
        authRunning_ = false;
    };
    try {
        ChatSettings s;
        { std::lock_guard lock(m_); s = settings_; }
        LoopbackServer srv;
        if (!srv.listen()) { finish("error", "Não foi possível abrir a porta local de retorno."); return; }
        const AuthRequest ar = buildGoogleAuthRequest(s.youtubeClientId, srv.redirectUri(), deps_.random);
        if (!deps_.openUrl || !deps_.openUrl(ar.url)) {
            finish("error", "Não foi possível abrir o navegador.");
            return;
        }
        { std::lock_guard lock(m_); authState_ = "waiting"; authMessage_ = "Conclua o login no navegador (até 3 min)."; }
        const std::string raw = srv.waitForRedirect(deps_.authTimeoutMs, kAuthHtml, &authCancel_);
        const RedirectResult rr = parseLoopbackRequest(raw);
        if (!rr.ok) { finish("error", authCancel_ ? "Autorização cancelada." : "Tempo esgotado esperando a autorização."); return; }
        if (!rr.error.empty()) { finish("error", "Acesso negado no Google (" + rr.error + ")."); return; }
        if (rr.state != ar.state) { finish("error", "Resposta de autorização inválida (state)."); return; }
        { std::lock_guard lock(m_); authState_ = "working"; authMessage_ = "Trocando o código por tokens..."; }
        std::string err;
        if (!tokens_.exchangeCode(rr.code, ar.verifier, srv.redirectUri(), std::chrono::steady_clock::now(), err)) {
            finish("error", "Falha na autorização: " + err);
            return;
        }
        finish("ok", "YouTube autorizado.");
        say("YouTube autorizado.");
    } catch (...) {
        finish("error", "Erro inesperado na autorização.");
    }
}

bool ChatService::handleCommand(const std::string& msg) {
    if (msg.compare(0, 4, "chat") != 0) return false;
    const auto a = splitCmd(msg, 3);
    if (a.empty()) return false;
    const std::string& c = a[0];
    const std::string arg = a.size() > 1 ? a[1] : std::string();
    const std::string rest = a.size() > 2 ? a[2] : std::string();
    Platform plat = Platform::Twitch;
    const bool havePlat = parsePlatform(arg, plat);
    std::uint64_t n = 0;

    if (c == "chatget") {
        postAll();
    } else if (c == "chatconnect" && havePlat) {
        setWanted(plat, true);
    } else if (c == "chatdisconnect" && havePlat) {
        setWanted(plat, false);
    } else if (c == "chatsave" && havePlat) {
        Json j;
        if (!hb::core::json::parse(rest, j) || !j.isObj()) { say("Configuração de chat inválida."); return true; }
        bool bad = false;
        {
            std::lock_guard lock(m_);
            switch (plat) {
                case Platform::YouTube: {
                    const std::string v = strField(j, "videoId"), cid = strField(j, "clientId");
                    if (!v.empty()) { const auto cv = cleanVideoId(v); if (cv.empty()) bad = true; else settings_.youtubeVideoId = cv; }
                    else settings_.youtubeVideoId.clear();
                    if (!cid.empty()) { const auto cc = cleanClientId(cid); if (cc.empty()) bad = true; else settings_.youtubeClientId = cc; }
                    else settings_.youtubeClientId.clear();
                    break;
                }
                case Platform::Facebook: {
                    const std::string v = strField(j, "liveVideoId");
                    if (!v.empty()) { const auto cv = cleanGraphId(v); if (cv.empty()) bad = true; else settings_.facebookLiveVideoId = cv; }
                    else settings_.facebookLiveVideoId.clear();
                    break;
                }
                case Platform::Twitch: {
                    const std::string v = strField(j, "channel");
                    if (!v.empty()) { const auto cv = cleanTwitchChannel(v); if (cv.empty()) bad = true; else settings_.twitchChannel = cv; }
                    else settings_.twitchChannel.clear();
                    break;
                }
                case Platform::Instagram: break;
            }
        }
        if (bad) { say("Valor inválido: confira o ID/canal informado."); postAll(); return true; }
        if (plat == Platform::YouTube) saveSecret(kYtSecretKey, strField(j, "clientSecret"));
        if (plat == Platform::Facebook) saveSecret(kFbTokenKey, strField(j, "token"));
        applySettings();
        persistSettings();
        say(std::string("Configuração de ") + platformLabel(plat) + " salva.");
        postAll();
    } else if (c == "chatclear" && havePlat) {
        if (plat == Platform::YouTube) { tokens_.signOut(); secrets_->erase(kYtSecretKey); }
        if (plat == Platform::Facebook) secrets_->erase(kFbTokenKey);
        applySettings();
        say(std::string("Credenciais de ") + platformLabel(plat) + " removidas.");
        postAll();
    } else if (c == "chatauth" && arg == "youtube") {
        startAuth();
    } else if (c == "chatauthcancel") {
        authCancel_ = true;
    } else if (c == "chatpin" && parseSeq(arg, n)) {
        if (!hub_.pin(n)) say("Mensagem não encontrada (já saiu do histórico).");
    } else if (c == "chatunpin") {
        hub_.clearPin();
    } else if (c == "chatmute" && parseSeq(arg, n)) {
        if (hub_.muteUserOf(n)) { persistSettings(); say("Usuário silenciado localmente."); }
    } else if (c == "chatunmute" && !arg.empty()) {
        hub_.unmuteUser(arg + (rest.empty() ? "" : "|" + rest));
        persistSettings();
    } else if (c == "chatclearfeed") {
        hub_.clearMessages();
        postAll();
    } else if (c == "chatopt" && arg == "persist") {
        { std::lock_guard lock(m_); settings_.persistMessages = rest == "1"; }
        applySettings();
        persistSettings();
        postAll();
    } else {
        return false;
    }
    cv_.notify_all();
    return true;
}

}  // namespace hb::chat
