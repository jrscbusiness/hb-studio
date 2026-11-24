#include "remote/service.hpp"

#include "core/json.hpp"
#include "remote/lan.hpp"

namespace hb::remote {

extern const unsigned char kMobilePageData[];
extern const std::size_t kMobilePageSize;

RemoteService::RemoteService(RemoteServer::CommandSink sink, RemoteServer::LogSink log, ServiceOptions opt)
    : opt_(std::move(opt)), sink_(std::move(sink)), log_(std::move(log)), auth_(systemRandom(), opt_.auth) {}

RemoteService::~RemoteService() { disable(); }

bool RemoteService::enabled() const {
    std::lock_guard lock(mu_);
    return server_ && server_->running();
}

std::uint16_t RemoteService::port() const {
    std::lock_guard lock(mu_);
    return server_ ? server_->port() : 0;
}

bool RemoteService::enable(std::string& error) {
    std::lock_guard lock(mu_);
    if (server_ && server_->running()) return true;
    std::vector<std::string> addrs = opt_.bindOverride.empty() ? listLanAddresses() : opt_.bindOverride;
    if (addrs.empty()) {
        error = "Nenhuma rede local (Wi-Fi/Ethernet) detectada. Conecte o computador a uma rede e tente de novo.";
        lastError_ = error;
        return false;
    }
    auth_.revokeAll();  // fresh PIN, no leftover sessions from a previous run
    ServerConfig cfg = opt_.server;
    cfg.bindAddresses = addrs;
    cfg.pageHtml.assign(reinterpret_cast<const char*>(kMobilePageData), kMobilePageSize);
    std::string err;
    for (int attempt = 0; attempt < 11; ++attempt) {
        cfg.port = (attempt < 10 && opt_.preferredPort != 0) ? static_cast<std::uint16_t>(opt_.preferredPort + attempt) : 0;
        auto s = std::make_unique<RemoteServer>(cfg, auth_, sink_, log_);
        if (s->start(err)) {
            server_ = std::move(s);
            urls_.clear();
            for (const auto& a : server_->boundAddresses()) urls_.push_back("http://" + a + ":" + std::to_string(server_->port()) + "/");
            lastError_.clear();
            return true;
        }
        if (cfg.port == 0) break;
    }
    error = "Não foi possível abrir a porta do controle remoto: " + err;
    lastError_ = error;
    return false;
}

void RemoteService::disable() {
    std::unique_ptr<RemoteServer> s;
    {
        std::lock_guard lock(mu_);
        s = std::move(server_);
        urls_.clear();
    }
    if (s) s->stop();
    auth_.revokeAll();
}

std::string RemoteService::handleUi(const std::string& message) {
    try {
        const auto p1 = message.find('|');
        const std::string rest = p1 == std::string::npos ? std::string() : message.substr(p1 + 1);
        const auto p2 = rest.find('|');
        const std::string sub = rest.substr(0, p2);
        const std::string arg = p2 == std::string::npos ? std::string() : rest.substr(p2 + 1);
        if (sub == "on") {
            std::string err;
            return enable(err) ? "Controle remoto ligado." : err;
        }
        if (sub == "off") {
            disable();
            return "Controle remoto desligado; todos os aparelhos foram desconectados.";
        }
        if (sub == "newpin") {
            auth_.newPin();
            return "";
        }
        if (sub == "revoke") {
            auth_.revokeAll();
            return "Todos os aparelhos foram desconectados e o PIN foi trocado.";
        }
        if (sub == "kick" && !arg.empty() && arg.size() < 18 && arg.find_first_not_of("0123456789") == std::string::npos) {
            auth_.revoke(std::stoull(arg));
            return "";
        }
    } catch (...) {
    }
    return "";
}

std::string RemoteService::statusJson() {
    std::lock_guard lock(mu_);
    const bool on = server_ && server_->running();
    std::string o = std::string("{\"t\":\"remote\",\"enabled\":") + (on ? "true" : "false");
    o += ",\"err\":\"" + core::json::escape(lastError_) + "\"";
    if (on) {
        const auto now = std::chrono::steady_clock::now();
        o += ",\"port\":" + std::to_string(server_->port()) + ",\"urls\":[";
        for (std::size_t i = 0; i < urls_.size(); ++i) o += std::string(i ? "," : "") + "\"" + core::json::escape(urls_[i]) + "\"";
        o += "],\"pin\":\"" + auth_.pin() + "\",\"lockedMs\":" + std::to_string(auth_.lockedFor(now).count()) + ",\"devices\":[";
        const auto live = server_->connectedDevices();
        bool first = true;
        for (const auto& s : auth_.sessions(now)) {
            bool online = false, thumbs = false;
            for (const auto& l : live)
                if (l.sessionId == s.id) {
                    online = true;
                    thumbs = l.thumbs;
                }
            o += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(s.id) + ",\"name\":\"" + core::json::escape(s.name) +
                 "\",\"ip\":\"" + core::json::escape(s.ip) + "\",\"online\":" + (online ? "true" : "false") +
                 ",\"thumbs\":" + (thumbs ? "true" : "false") + ",\"idle\":" + std::to_string(s.idleSeconds) + "}";
            first = false;
        }
        o += "]";
    }
    return o + "}";
}

bool RemoteService::wantsState() const {
    std::lock_guard lock(mu_);
    return server_ && server_->running() && server_->clientCount() > 0;
}

void RemoteService::pushState(const RemoteState& s) {
    std::lock_guard lock(mu_);
    if (server_ && server_->running()) server_->broadcastState(toJson(s));
}

bool RemoteService::wantsThumbs() const {
    std::lock_guard lock(mu_);
    return server_ && server_->running() && server_->wantsThumbs();
}

void RemoteService::pushThumb(int kind, const std::vector<std::uint8_t>& jpeg) {
    std::lock_guard lock(mu_);
    if (server_ && server_->running()) server_->broadcastThumb(kind, jpeg);
}

}  // namespace hb::remote
