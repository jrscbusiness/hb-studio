#include "chat/net_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace hb::chat {
namespace {

class NetBackedChatHttp : public IChatHttp {
public:
    explicit NetBackedChatHttp(std::shared_ptr<hb::net::IHttpClient> c) : client_(std::move(c)) {}

    HttpResponse send(const HttpRequest& req) override {
        namespace net = hb::net;
        HttpResponse resp;
        if (!client_) { resp.error = "HTTP client unavailable"; return resp; }
        net::HttpRequest n;
        n.method = req.method;
        n.url = req.url;
        n.headers = req.headers;
        n.body = req.body;
        n.timeouts = net::Timeouts::all(std::clamp(req.timeoutMs, 1000, 60000));
        n.maxResponseBytes = 4u * 1024 * 1024;
        n.overflow = net::Overflow::Fail;
        const net::HttpResponse r = client_->send(n);
        resp.status = r.status;
        resp.body = r.body;
        if (r.error == net::ErrorKind::None) {
            resp.transportOk = true;
        } else if (r.error == net::ErrorKind::InvalidRequest && r.errorText.find("URL") != std::string::npos) {
            resp.error = "invalid URL";
        } else if (r.error == net::ErrorKind::TooLarge) {
            resp.error = "response too large";
        } else {
            resp.error = r.errorText;
        }
        return resp;
    }

private:
    std::shared_ptr<hb::net::IHttpClient> client_;
};

std::filesystem::path keyPath(const std::string& dir, const std::string& key) {
    std::string f;
    for (char c : key) f += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    return std::filesystem::path(std::u8string(dir.begin(), dir.end())) / std::filesystem::path(f + ".bin");
}

class FileSecretStore : public ISecretStore {
public:
    FileSecretStore(std::string dir, std::shared_ptr<hb::net::ISecretStore> protector)
        : dir_(std::move(dir)), protector_(std::move(protector)) {}

    bool put(const std::string& key, const std::string& value) override {
        if (key.empty() || key.size() > 64 || !protector_) return false;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(std::u8string(dir_.begin(), dir_.end())), ec);
        std::string blob;
        if (!protector_->protect(value, blob)) return false;
        const auto path = keyPath(dir_, key);
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
            f.flush();
            if (!f) {
                f.close();
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
        std::filesystem::rename(tmp, path, ec);  // replaces an existing file
        if (ec) std::filesystem::remove(tmp, ec);
        return !ec;
    }

    bool get(const std::string& key, std::string& out) const override {
        if (!protector_) return false;
        std::string blob;
        {
            std::ifstream f(keyPath(dir_, key), std::ios::binary | std::ios::ate);
            if (!f) return false;
            const std::streamoff size = f.tellg();
            if (size <= 0 || size >= 64 * 1024) return false;
            blob.resize(static_cast<std::size_t>(size));
            f.seekg(0);
            f.read(&blob[0], size);
            if (!f) return false;
        }
        return protector_->unprotect(blob, out);
    }

    bool erase(const std::string& key) override {
        std::error_code ec;
        return std::filesystem::remove(keyPath(dir_, key), ec) && !ec;
    }

private:
    std::string dir_;
    std::shared_ptr<hb::net::ISecretStore> protector_;
};

}  // namespace

std::unique_ptr<IChatHttp> makeChatHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client) {
    return std::make_unique<NetBackedChatHttp>(std::move(client));
}

std::unique_ptr<ISecretStore> makeFileSecretStore(const std::string& dir, std::shared_ptr<hb::net::ISecretStore> protector) {
    return std::make_unique<FileSecretStore>(dir, std::move(protector));
}

}  // namespace hb::chat
