#include "cloud/net_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "cloud/hash_util.hpp"
#include "core/json.hpp"

namespace hb::cloud {
namespace {

bool iequals(const std::string& a, const char* b) {
    const std::size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

class NetBackedCloudHttp : public ICloudHttp {
public:
    explicit NetBackedCloudHttp(std::shared_ptr<hb::net::IHttpClient> c) : client_(std::move(c)) {}

    Result<HttpResponse> send(const HttpRequest& req) override {
        namespace net = hb::net;
        if (!client_) return makeError(ErrorKind::Transient, "Falha de rede (cliente HTTP indisponível)");
        net::HttpRequest n;
        n.method = req.method;
        n.url = req.url;
        for (const auto& h : req.headers) {
            if (iequals(h.first, "host") || iequals(h.first, "content-length")) continue;  // added by the transport
            n.headers.push_back(h);
        }
        // Never follow redirects: Drive's 308 "resume incomplete" must reach us as-is and a wrong S3 region
        // must surface as an error instead of silently re-sending a multi-MiB body.
        n.redirects = net::RedirectPolicy::Never;
        const int t = static_cast<int>(std::min<long long>(req.timeout.count(), 1800000));
        n.timeouts = net::Timeouts{15000, 30000, 60000, t};
        n.maxResponseBytes = 4 * 1024 * 1024;  // cap: ignore the excess
        n.overflow = net::Overflow::Truncate;
        if (req.gate) {
            // Body in 32 KiB slices, each one cleared by the gate first (bandwidth cap / pause / cancel).
            ITransferGate* gate = req.gate;
            const std::string* body = &req.body;
            auto off = std::make_shared<std::size_t>(0);
            n.bodySourceLength = body->size();
            n.bodySourceChunk = 32 * 1024;
            n.bodySource = [gate, body, off](char* buf, std::size_t cap) -> std::size_t {
                const std::size_t k = std::min(cap, body->size() - *off);
                if (k == 0) return 0;
                if (!gate->acquire(k)) return net::kBodyAbort;
                std::memcpy(buf, body->data() + *off, k);
                *off += k;
                return k;
            };
        } else {
            n.body = req.body;
        }
        const net::HttpResponse r = client_->send(n);
        switch (r.error) {
            case net::ErrorKind::None:
                break;
            case net::ErrorKind::InvalidRequest:
                if (r.errorText.find("body") != std::string::npos) return makeError(ErrorKind::Permanent, "Corpo da requisição grande demais");
                if (r.errorText.find("scheme") != std::string::npos) return makeError(ErrorKind::Permanent, "Esquema de URL não suportado");
                return makeError(ErrorKind::Permanent, "URL inválida");
            case net::ErrorKind::Cancelled:
                return makeError(ErrorKind::Cancelled, "Envio interrompido");
            default:
                return makeError(ErrorKind::Transient, "Falha de rede (" + (r.errorStage.empty() ? r.errorText : r.errorStage) +
                                                           ", erro WinHTTP " + std::to_string(r.nativeError) + ")");
        }
        HttpResponse out;
        out.status = r.status;
        out.headers = r.headers;
        out.body = r.body;
        return out;
    }

private:
    std::shared_ptr<hb::net::IHttpClient> client_;
};

class FileSecretStore : public ISecretStore {
public:
    FileSecretStore(std::string path, std::shared_ptr<hb::net::ISecretStore> protector)
        : path_(std::move(path)), protector_(std::move(protector)) {
        load();
    }

    bool put(const std::string& name, const std::string& value) override {
        std::lock_guard lock(mu_);
        std::string blob;
        if (!protector_ || !protector_->protect(value, blob)) return false;
        items_[name] = base64Encode(blob);
        return save();
    }

    std::optional<std::string> get(const std::string& name) override {
        std::lock_guard lock(mu_);
        auto it = items_.find(name);
        if (it == items_.end() || !protector_) return std::nullopt;
        const std::string blob = base64Decode(it->second);
        if (blob.empty()) return std::nullopt;
        std::string v;
        if (!protector_->unprotect(blob, v)) return std::nullopt;
        return v;
    }

    bool erase(const std::string& name) override {
        std::lock_guard lock(mu_);
        if (items_.erase(name) == 0) return false;
        return save();
    }

private:
    std::filesystem::path p() const { return std::filesystem::path(std::u8string(path_.begin(), path_.end())); }
    void load() {
        std::string text;
        {
            std::ifstream f(p(), std::ios::binary);
            if (!f) return;
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
        hb::core::json::Value r;
        if (!hb::core::json::parse(text, r) || !r.isObj()) return;
        const auto* items = r.get("items");
        if (!items || !items->isObj()) return;
        for (const auto& kv : items->o)
            if (kv.second.isStr()) items_[kv.first] = kv.second.s;
    }
    bool save() {
        hb::core::json::Value root, items;
        root.t = hb::core::json::Value::T::Obj;
        items.t = hb::core::json::Value::T::Obj;
        for (const auto& kv : items_) {
            hb::core::json::Value v;
            v.t = hb::core::json::Value::T::Str;
            v.s = kv.second;
            items.o[kv.first] = v;
        }
        hb::core::json::Value ver;
        ver.t = hb::core::json::Value::T::Num;
        ver.n = 1;
        root.o["v"] = ver;
        root.o["items"] = items;
        const std::string tmp = path_ + ".tmp";
        const auto tp = std::filesystem::path(std::u8string(tmp.begin(), tmp.end()));
        {
            std::ofstream f(tp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << hb::core::json::dump(root, true);
            if (!f.good()) return false;
        }
        std::error_code ec;
        std::filesystem::rename(tp, p(), ec);
        return !ec;
    }
    std::mutex mu_;
    std::string path_;
    std::shared_ptr<hb::net::ISecretStore> protector_;
    std::map<std::string, std::string> items_;
};

}  // namespace

std::shared_ptr<ICloudHttp> makeCloudHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client) {
    return std::make_shared<NetBackedCloudHttp>(std::move(client));
}

std::shared_ptr<ISecretStore> makeFileSecretStore(const std::string& path, std::shared_ptr<hb::net::ISecretStore> protector) {
    return std::make_shared<FileSecretStore>(path, std::move(protector));
}

}  // namespace hb::cloud
