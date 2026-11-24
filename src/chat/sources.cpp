#include "chat/sources.hpp"

#include <algorithm>
#include <cctype>

#include "chat/irc.hpp"
#include "chat/parsers.hpp"
#include "core/redact.hpp"

namespace hb::chat {

std::int64_t systemEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool RecentIds::insert(const std::string& id) {
    if (!set_.insert(id).second) return false;
    order_.push_back(id);
    while (order_.size() > cap_) {
        set_.erase(order_.front());
        order_.pop_front();
    }
    return true;
}

SourceStatus SourceBase::status() const {
    std::lock_guard lock(sm_);
    return st_;
}
void SourceBase::setState(SourceState s, std::string detail, std::string error) {
    std::lock_guard lock(sm_);
    st_.state = s;
    st_.detail = std::move(detail);
    if (!error.empty()) st_.lastError = hb::core::redactSecrets(error);
}
void SourceBase::addQuota(int units) {
    std::lock_guard lock(sm_);
    st_.quotaUnits += units;
}

namespace {
bool idChars(const std::string& s, std::size_t maxLen) {
    if (s.empty() || s.size() > maxLen) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
Millis clampMs(std::int64_t v, std::int64_t lo, std::int64_t hi) { return Millis(std::clamp(v, lo, hi)); }
}  // namespace

// ================================================================ YouTube
void YouTubeSource::configure(YouTubeConfig cfg) {
    std::lock_guard lock(cfgM_);
    pending_ = std::move(cfg);
    dirty_ = true;
}

void YouTubeSource::applyPending() {
    std::lock_guard lock(cfgM_);
    if (!dirty_) return;
    cfg_ = pending_;
    dirty_ = false;
    chatId_.clear();
    pageToken_.clear();
    firstPage_ = true;
    seen_.clear();
    nextPollAt_ = TimePoint{};
    backoff_.onSuccess();
    if (wanted_) setState(SourceState::Connecting, "Reconectando com a nova configuração");
}

void YouTubeSource::connect() {
    applyPending();
    wanted_ = true;
    chatId_.clear();
    pageToken_.clear();
    firstPage_ = true;
    seen_.clear();
    nextPollAt_ = TimePoint{};
    backoff_.onSuccess();
    setState(SourceState::Connecting, "Conectando ao YouTube...");
}

void YouTubeSource::disconnect() {
    wanted_ = false;
    setState(SourceState::Disconnected, "");
}

void YouTubeSource::fail(TimePoint now, const std::string& what, bool permanent, SourceState state) {
    if (permanent) {
        wanted_ = false;
        setState(state == SourceState::Backoff ? SourceState::Failed : state, what, what);
        return;
    }
    const Millis d = backoff_.onFailure();
    nextPollAt_ = now + d;
    setState(SourceState::Backoff, what + " (nova tentativa em " + std::to_string(d.count() / 1000) + " s)", what);
}

std::vector<ChatMessage> YouTubeSource::poll(TimePoint now) {
    applyPending();
    if (!wanted_ || now < nextPollAt_) return {};

    std::string token, err;
    bool needsAuth = false;
    if (!tokens_.accessToken(now, token, err, needsAuth)) {
        if (needsAuth) {
            nextPollAt_ = now + Millis(5000);
            setState(SourceState::NeedsAuth, "Autorize o acesso ao YouTube (botão AUTORIZAR)", err);
        } else {
            fail(now, err, false);
        }
        return {};
    }
    const std::vector<std::pair<std::string, std::string>> auth = {{"Authorization", "Bearer " + token},
                                                                    {"Accept", "application/json"}};

    // Common error handling for both calls. Returns after updating state.
    auto handleError = [&](const HttpResponse& r) {
        if (!r.transportOk) { fail(now, "rede: " + r.error, false); return; }
        if (r.status == 401) {
            tokens_.invalidate();
            if (backoff_.failures() >= 2) {
                nextPollAt_ = now + Millis(5000);
                setState(SourceState::NeedsAuth, "O YouTube recusou o token; autorize novamente", "HTTP 401");
                return;
            }
            fail(now, "token recusado (401); renovando", false);
            return;
        }
        ApiError e;
        parseGoogleError(r.body, e);
        const std::string why = e.reason.empty() ? ("HTTP " + std::to_string(r.status)) : e.reason;
        if (e.reason == "liveChatEnded") {
            wanted_ = false;
            setState(SourceState::Ended, "O chat ao vivo terminou", why);
        } else if (e.reason == "liveChatDisabled") {
            fail(now, "O chat está desativado nesta transmissão", true);
        } else if (e.reason == "liveChatNotFound" || r.status == 404) {
            chatId_.clear();
            fail(now, "Chat não encontrado: confira o ID do vídeo", true);
        } else if (e.reason == "quotaExceeded" || e.reason == "dailyLimitExceeded") {
            nextPollAt_ = now + std::chrono::minutes(30);
            setState(SourceState::Backoff, "Cota diária da API esgotada (renova à meia-noite, horário do Pacífico)", why);
        } else if (e.reason == "rateLimitExceeded" || e.reason == "userRateLimitExceeded" || r.status >= 500) {
            fail(now, why, false);
        } else if (e.reason == "forbidden" || r.status == 403) {
            fail(now, "Sem permissão para ler este chat (use a conta dona da transmissão): " + why, true);
        } else if (r.status >= 400 && r.status < 500) {
            fail(now, "Erro da API: " + why + (e.message.empty() ? "" : " - " + e.message), true);
        } else {
            fail(now, why, false);
        }
    };

    if (chatId_.empty()) chatId_ = cfg_.liveChatId;
    if (chatId_.empty()) {
        if (!idChars(cfg_.videoId, 32)) {
            fail(now, "Informe o ID do vídeo da transmissão", true);
            return {};
        }
        HttpRequest req;
        req.url = "https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=" + urlEncode(cfg_.videoId);
        req.headers = auth;
        const HttpResponse r = http_.send(req);
        addQuota(kVideosCost);
        if (!r.transportOk || r.status != 200) { handleError(r); return {}; }
        std::string id;
        if (!parseYouTubeActiveChatId(r.body, id)) { fail(now, "resposta inesperada do YouTube", false); return {}; }
        if (id.empty()) {
            nextPollAt_ = now + Millis(20000);
            setState(SourceState::Connecting, "Aguardando a transmissão começar (vídeo sem chat ao vivo ativo)");
            return {};
        }
        chatId_ = id;
    }

    HttpRequest req;
    req.url = "https://www.googleapis.com/youtube/v3/liveChat/messages?part=snippet%2CauthorDetails&maxResults=200&liveChatId=" +
              urlEncode(chatId_) + (pageToken_.empty() ? "" : "&pageToken=" + urlEncode(pageToken_));
    req.headers = auth;
    const HttpResponse r = http_.send(req);
    addQuota(kListCost);
    if (!r.transportOk || r.status != 200) { handleError(r); return {}; }
    YouTubeChatPage page;
    if (!parseYouTubeChatPage(r.body, epochMs_(), page)) { fail(now, "resposta inesperada do YouTube", false); return {}; }

    backoff_.onSuccess();
    if (!page.nextPageToken.empty()) pageToken_ = page.nextPageToken;
    const std::int64_t wait = page.pollingIntervalMillis > 0 ? page.pollingIntervalMillis : 5000;
    nextPollAt_ = now + clampMs(wait, 1000, 60000);

    std::vector<ChatMessage> out;
    for (auto& m : page.messages)
        if (seen_.insert(m.id) && !(firstPage_ && cfg_.skipBacklog)) out.push_back(std::move(m));
    firstPage_ = false;
    if (page.offline) {
        wanted_ = false;
        setState(SourceState::Ended, "A transmissão terminou");
    } else {
        setState(SourceState::Connected, "Conectado ao chat ao vivo");
    }
    return out;
}

// ================================================================ Facebook
void FacebookSource::configure(FacebookConfig cfg) {
    std::lock_guard lock(cfgM_);
    pending_ = std::move(cfg);
    dirty_ = true;
}

void FacebookSource::applyPending() {
    std::lock_guard lock(cfgM_);
    if (!dirty_) return;
    cfg_ = pending_;
    cfg_.pollIntervalMs = std::clamp(cfg_.pollIntervalMs, 2000, 60000);
    dirty_ = false;
    firstPage_ = true;
    seen_.clear();
    nextPollAt_ = TimePoint{};
    backoff_.onSuccess();
    if (wanted_) setState(SourceState::Connecting, "Reconectando com a nova configuração");
}

void FacebookSource::connect() {
    applyPending();
    wanted_ = true;
    firstPage_ = true;
    seen_.clear();
    nextPollAt_ = TimePoint{};
    backoff_.onSuccess();
    setState(SourceState::Connecting, "Conectando ao Facebook...");
}

void FacebookSource::disconnect() {
    wanted_ = false;
    setState(SourceState::Disconnected, "");
}

std::vector<ChatMessage> FacebookSource::poll(TimePoint now) {
    applyPending();
    if (!wanted_ || now < nextPollAt_) return {};
    auto fail = [&](const std::string& what, bool permanent, SourceState st = SourceState::Failed) {
        if (permanent) { wanted_ = false; setState(st, what, what); return; }
        const Millis d = backoff_.onFailure();
        nextPollAt_ = now + d;
        setState(SourceState::Backoff, what + " (nova tentativa em " + std::to_string(d.count() / 1000) + " s)", what);
    };
    const std::string token = tokenFn_ ? tokenFn_() : std::string();
    if (token.empty()) {
        nextPollAt_ = now + Millis(5000);
        setState(SourceState::NeedsAuth, "Informe o token de acesso da Página do Facebook");
        return {};
    }
    if (!idChars(cfg_.liveVideoId, 40)) { fail("Informe o ID do vídeo ao vivo", true); return {}; }

    HttpRequest req;
    // The token travels in the query string (Graph API convention); URLs are never logged.
    req.url = "https://graph.facebook.com/v21.0/" + urlEncode(cfg_.liveVideoId) +
              "/comments?order=reverse_chronological&live_filter=no_filter&limit=100&fields=" +
              urlEncode("id,message,created_time,from{id,name,picture.type(small){url}}") +
              "&access_token=" + urlEncode(token);
    req.headers = {{"Accept", "application/json"}};
    const HttpResponse r = http_.send(req);
    if (!r.transportOk) { fail("rede: " + r.error, false); return {}; }
    if (r.status != 200) {
        GraphError e;
        parseGraphError(r.body, e);
        const std::string why = "Graph API " + std::to_string(e.code ? e.code : r.status) + (e.message.empty() ? "" : ": " + e.message);
        if (e.code == 190) {
            nextPollAt_ = now + Millis(5000);
            setState(SourceState::NeedsAuth, "Token do Facebook inválido ou expirado", why);
        } else if (e.code == 4 || e.code == 17 || e.code == 32 || e.code == 341 || e.code == 613) {
            nextPollAt_ = now + std::chrono::minutes(2);
            setState(SourceState::Backoff, "Limite de requisições do Facebook; aguardando 2 min", why);
        } else if (e.code == 10 || e.code == 200 || e.code == 100 || e.code == 803) {
            fail("Sem permissão ou ID inválido (veja docs/CHAT.md): " + why, true);
        } else if (r.status >= 500 || e.code == 1 || e.code == 2) {
            fail(why, false);
        } else if (r.status >= 400 && r.status < 500) {
            fail(why, true);
        } else {
            fail(why, false);
        }
        return {};
    }
    GraphCommentsPage page;
    if (!parseFacebookComments(r.body, epochMs_(), page)) { fail("resposta inesperada do Facebook", false); return {}; }
    backoff_.onSuccess();
    nextPollAt_ = now + Millis(cfg_.pollIntervalMs);
    setState(SourceState::Connected, "Conectado (consulta a cada " + std::to_string(cfg_.pollIntervalMs / 1000) + " s)");
    std::vector<ChatMessage> out;
    for (auto it = page.messages.rbegin(); it != page.messages.rend(); ++it)  // newest-first -> oldest-first
        if (seen_.insert(it->id) && !(firstPage_ && cfg_.skipBacklog)) out.push_back(std::move(*it));
    firstPage_ = false;
    return out;
}

// ================================================================ Instagram
void InstagramWebhookSource::refreshStatus() {
    std::lock_guard lock(qm_);
    setState(SourceState::Unavailable,
             received_ == 0
                 ? "Indisponível: o Instagram não oferece leitura do chat ao vivo por consulta (polling). Só existe o webhook "
                   "live_comments (endpoint HTTPS público + revisão do app Meta), que este aplicativo não hospeda."
                 : "Webhook live_comments: " + std::to_string(received_) + " comentários recebidos via relay");
}

int InstagramWebhookSource::feedWebhook(const std::string& body) {
    std::vector<ChatMessage> msgs;
    if (!parseInstagramLiveWebhook(body, epochMs_(), msgs)) return 0;
    std::lock_guard lock(qm_);
    for (auto& m : msgs)
        if (queue_.size() < 1000) queue_.push_back(std::move(m));
    received_ += msgs.size();
    return static_cast<int>(msgs.size());
}

std::vector<ChatMessage> InstagramWebhookSource::poll(TimePoint) {
    std::vector<ChatMessage> out;
    std::size_t n;
    {
        std::lock_guard lock(qm_);
        out.swap(queue_);
        n = received_;
    }
    if (!out.empty()) {
        setState(SourceState::Unavailable, "Webhook live_comments: " + std::to_string(n) + " comentários recebidos via relay");
    }
    return out;
}

// ================================================================ Twitch
bool TwitchSource::validChannel(const std::string& c) {
    if (c.empty() || c.size() > 25) return false;
    for (unsigned char ch : c)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    return true;
}

void TwitchSource::configure(TwitchConfig cfg) {
    std::lock_guard lock(cfgM_);
    pending_ = std::move(cfg);
    dirty_ = true;
}

void TwitchSource::applyPending() {
    std::lock_guard lock(cfgM_);
    if (!dirty_) return;
    cfg_ = pending_;
    dirty_ = false;
    std::string c = cfg_.channel;
    if (!c.empty() && c[0] == '#') c.erase(0, 1);
    for (auto& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    channel_ = c;
    if (open_) { sock_.close(); open_ = false; }
    nextAttemptAt_ = TimePoint{};
    backoff_.onSuccess();
    if (wanted_) setState(SourceState::Connecting, "Reconectando com a nova configuração");
}

void TwitchSource::connect() {
    applyPending();
    wanted_ = true;
    if (open_) { sock_.close(); open_ = false; }
    nextAttemptAt_ = TimePoint{};
    backoff_.onSuccess();
    setState(SourceState::Connecting, "Conectando ao Twitch...");
}

void TwitchSource::disconnect() {
    wanted_ = false;
    if (open_) { sock_.close(); open_ = false; }
    setState(SourceState::Disconnected, "");
}

void TwitchSource::drop(TimePoint now, const std::string& why) {
    if (open_) { sock_.close(); open_ = false; }
    const Millis d = backoff_.onFailure();
    nextAttemptAt_ = now + d;
    setState(SourceState::Backoff, why + " (nova tentativa em " + std::to_string(d.count() / 1000) + " s)", why);
}

std::vector<ChatMessage> TwitchSource::poll(TimePoint now) {
    applyPending();
    std::vector<ChatMessage> out;
    if (!wanted_) return out;
    if (!open_) {
        if (now < nextAttemptAt_) return out;
        if (!validChannel(channel_)) {
            wanted_ = false;
            setState(SourceState::Failed, "Informe o canal da Twitch (somente letras minúsculas, números e _)", "canal inválido");
            return out;
        }
        std::string err;
        if (!sock_.connect(kUrl, err)) { drop(now, "falha ao conectar: " + err); return out; }
        open_ = true;
        lastActivity_ = now;
        // Anonymous login: justinfan<digits> needs no token. Capabilities give us tags (badges, ids, emotes).
        const bool sent = sock_.sendText("CAP REQ :twitch.tv/tags twitch.tv/commands") && sock_.sendText("PASS SCHMOOCHIES") &&
                          sock_.sendText("NICK justinfan" + std::to_string(10000 + epochMs_() % 80000)) &&
                          sock_.sendText("JOIN #" + channel_);
        if (!sent) { drop(now, "falha ao enviar o login"); return out; }
        setState(SourceState::Connecting, "Entrando em #" + channel_);
        return out;
    }

    for (int n = 0; n < 500; ++n) {
        std::string frame;
        const auto r = sock_.receive(frame, 0);
        if (r == IChatSocket::Recv::Timeout) break;
        if (r == IChatSocket::Recv::Closed) { drop(now, "conexão encerrada pelo servidor"); return out; }
        lastActivity_ = now;
        for (const auto& line : splitIrcLines(frame)) {
            IrcMessage m;
            if (!parseIrcLine(line, m)) continue;
            if (m.command == "PING") {
                sock_.sendText("PONG :" + (m.params.empty() ? std::string("tmi.twitch.tv") : m.params.back()));
            } else if (m.command == "RECONNECT") {
                drop(now, "o Twitch pediu para reconectar");
                backoff_.onSuccess();
                nextAttemptAt_ = now + Millis(1000);
                return out;
            } else if (m.command == "JOIN" || m.command == "366" || m.command == "001") {
                if (m.command != "001") {
                    backoff_.onSuccess();
                    setState(SourceState::Connected, "Conectado a #" + channel_ + " (leitura anônima)");
                }
            } else if (m.command == "NOTICE") {
                const auto it = m.tags.find("msg-id");
                const std::string text = m.params.empty() ? std::string() : m.params.back();
                if (it != m.tags.end() && (it->second == "msg_channel_suspended" || it->second == "msg_banned")) {
                    wanted_ = false;
                    if (open_) { sock_.close(); open_ = false; }
                    setState(SourceState::Failed, "O Twitch recusou o canal: " + sanitizeText(text, 120), text);
                    return out;
                }
            } else if (m.command == "PRIVMSG") {
                if (auto c = twitchToChatMessage(m, epochMs_())) out.push_back(std::move(*c));
            }
        }
    }
    if (open_ && now - lastActivity_ > std::chrono::minutes(7)) drop(now, "sem atividade por 7 min");
    return out;
}

}  // namespace hb::chat
