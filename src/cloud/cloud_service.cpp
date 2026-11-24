#include "cloud/cloud_service.hpp"

#include <algorithm>
#include <random>
#include <sstream>

#include "cloud/gdrive_provider.hpp"
#include "cloud/loopback_listener.hpp"
#include "cloud/s3_provider.hpp"
#include "core/json.hpp"

namespace hb::cloud {

using Json = hb::core::json::Value;

namespace {

std::string esc(const std::string& s) { return hb::core::json::escape(s); }
const char* b(bool v) { return v ? "true" : "false"; }

std::string randomBytes(std::size_t n) {
    std::random_device rd;  // MSVC: rand_s (CSPRNG); libstdc++: hardware/urandom
    std::string o;
    for (std::size_t i = 0; i < n; ++i) o += static_cast<char>(rd() & 0xff);
    return o;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    return dir + ((last == '/' || last == '\\') ? "" : "/") + name;
}

const char* kLoginPage =
    "<!doctype html><meta charset=utf-8><title>HB Studio</title><body style=\"font-family:sans-serif;padding:40px\">"
    "<h2>HB Studio</h2><p>Autorização concluída. Você já pode fechar esta janela e voltar ao aplicativo.</p>";
const char* kLoginFail =
    "<!doctype html><meta charset=utf-8><title>HB Studio</title><body style=\"font-family:sans-serif;padding:40px\">"
    "<h2>HB Studio</h2><p>Não foi possível concluir a autorização. Volte ao aplicativo e tente de novo.</p>";

}  // namespace

CloudService::CloudService(CloudServiceDeps deps) : d_(std::move(deps)) {
    if (!d_.fs) d_.fs = makeStdFileSystem();
    if (!d_.random) d_.random = randomBytes;
    cfgPath_ = joinPath(d_.dataDir, "cloud.json");
    loadSettings(cfgPath_, s_);
    s_ = sanitize(s_);

    EngineOptions eo;
    eo.journalPath = joinPath(d_.dataDir, "cloud_journal.json");
    eo.fs = d_.fs;
    eo.wallNow = d_.wallNow;
    eo.monoNow = d_.monoNow;
    eo.sleepFn = d_.sleepFn;
    eo.streamingActive = d_.streamingActive;
    eo.threadInit = d_.threadInit;
    eo.stableSeconds = s_.stableSeconds;
    engine_ = std::make_unique<UploadEngine>(eo);
    engine_->loadJournal();

    FolderWatchFeed::Options wo;
    wo.fs = d_.fs;
    wo.folder = d_.recordingsFolder;
    wo.wallNow = d_.wallNow;
    if (!wo.wallNow)
        wo.wallNow = [] { return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
    wo.stableSeconds = s_.stableSeconds;
    watch_ = std::make_unique<FolderWatchFeed>(wo);
    watch_->setSink([this](const ClosedFile& f) { onClosed(f); });
    if (d_.bus) {
        busFeed_ = std::make_unique<EventBusFeed>(*d_.bus);
        busFeed_->setSink([this](const ClosedFile& f) { onClosed(f); });
    }
    refreshHints();
    applySettings(true);
}

CloudService::~CloudService() { stop(); }

void CloudService::start() {
    if (!d_.startThreads) return;
    engine_->start();
    if (!scanner_.joinable()) scanner_ = std::thread([this] { scanLoop(); });
}

void CloudService::stop() {
    stopping_ = true;
    {
        std::lock_guard lock(scanMu_);
        scanCv_.notify_all();
    }
    if (scanner_.joinable()) scanner_.join();
    joinAsync();
    engine_->stop();
    engine_->saveJournal();
}

void CloudService::scanLoop() {
    while (!stopping_) {
        try {
            const CloudSettings s = settings();
            if (s.enabled && s.watchFolder) {
                std::lock_guard lock(watchMu_);
                watch_->poll();
            }
        } catch (...) {
        }
        std::unique_lock lock(scanMu_);
        scanCv_.wait_for(lock, std::chrono::seconds(5));
    }
}

CloudSettings CloudService::settings() const {
    std::lock_guard lock(mu_);
    return s_;
}

unsigned CloudService::version() const { return version_ + engine_->version(); }

void CloudService::refreshHints() {
    std::lock_guard lock(mu_);
    auto get = [&](const char* k) { auto v = d_.secrets->get(k); return v ? *v : std::string(); };
    const std::string a = get(kSecS3Access), sk = get(kSecS3Secret), gs = get(kSecGClientSecret), gr = get(kSecGRefresh);
    hintS3Access_ = maskHint(a);
    hintS3Secret_ = sk.empty() ? "" : "••••••••";   // no tail for the secret key itself
    hintGClient_ = gs.empty() ? "" : "••••••••";
    hintGRefresh_ = gr.empty() ? "" : "ok";
}

std::shared_ptr<ICloudProvider> CloudService::buildProvider(std::string& status) {
    const CloudSettings s = settings();
    auto get = [&](const char* k) { auto v = d_.secrets->get(k); return v ? *v : std::string(); };
    if (s.provider == "gdrive") {
        if (s.gClientId.empty()) { status = "Informe o Client ID do Google"; return nullptr; }
        const std::string refresh = get(kSecGRefresh);
        if (refresh.empty()) { status = "Conecte sua conta do Google Drive"; return nullptr; }
        GoogleOAuthConfig oc{s.gClientId, get(kSecGClientSecret)};
        auto mono = d_.monoNow ? d_.monoNow : std::function<double()>([] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        });
        auto tokens = std::make_shared<GoogleTokenSource>(oc, refresh, d_.http, mono);
        GDriveConfig gc;
        gc.folderName = s.gFolderName;
        gc.folderId = s.gFolderId;
        status = "Google Drive pronto";
        return std::make_shared<GDriveProvider>(gc, d_.http, tokens);
    }
    S3Config c;
    c.endpoint = s.s3Endpoint;
    c.region = s.s3Region;
    c.bucket = s.s3Bucket;
    c.prefix = s.s3Prefix;
    c.addressing = s.s3Addressing;
    c.accessKey = get(kSecS3Access);
    c.secretKey = get(kSecS3Secret);
    c.partSize = static_cast<std::uint64_t>(s.partSizeMiB) * 1024 * 1024;
    if (const std::string bad = validateS3Config(c); !bad.empty()) { status = bad; return nullptr; }
    status = "Armazenamento S3 configurado";
    return std::make_shared<S3Provider>(c, d_.http);
}

void CloudService::applySettings(bool rebuild) {
    const CloudSettings s = settings();
    engine_->setEnabled(s.enabled);
    engine_->setBandwidthMbps(s.bandwidthMbps);
    engine_->setPauseWhileStreaming(s.pauseWhileStreaming);
    engine_->setDeleteAfterUpload(s.deleteAfterUpload);
    if (rebuild) {
        std::string st;
        auto p = buildProvider(st);
        {
            std::lock_guard lock(mu_);
            status_ = st;
            provider_ = p;
        }
        engine_->setProvider(p);
    }
    ++version_;
}

void CloudService::onClosed(const ClosedFile& f) {
    const CloudSettings s = settings();
    if (!s.enabled) return;
    if (f.kind == "program" && !s.upProgram) return;
    if (f.kind == "iso-video" && !s.upIsoVideo) return;
    if (f.kind == "iso-audio" && !s.upIsoAudio) return;
    engine_->enqueue(f.path, f.kind, f.index, f.closedByProducer);
}

void CloudService::setTest(const std::string& st, const std::string& msg) {
    std::lock_guard lock(mu_);
    testState_ = st;
    testMsg_ = msg;
    ++version_;
}
void CloudService::setAuth(const std::string& st, const std::string& msg) {
    std::lock_guard lock(mu_);
    authState_ = st;
    authMsg_ = msg;
    ++version_;
}

void CloudService::joinAsync() {
    std::lock_guard lock(asyncMu_);
    if (async_.joinable()) async_.join();
}

void CloudService::startAsync(std::function<void()> fn) {
    std::lock_guard lock(asyncMu_);
    if (async_.joinable()) async_.join();  // the previous one is finished (asyncBusy_ is false)
    asyncBusy_ = true;
    async_ = std::thread([this, fn = std::move(fn)] {
        try {
            fn();
        } catch (...) {
        }
        asyncBusy_ = false;
    });
}

void CloudService::runTest() {
    setTest("running", "Testando a conexão…");
    std::shared_ptr<ICloudProvider> p;
    std::string st;
    {
        std::lock_guard lock(mu_);
        p = provider_;
        st = status_;
    }
    if (!p) return setTest("error", st.empty() ? "Configuração incompleta" : st);
    auto r = p->test();
    if (r.ok()) setTest("ok", r.value());
    else setTest("error", r.error().message);
    if (auto* g = dynamic_cast<GDriveProvider*>(p.get())) {  // persist the folder id found/created
        const std::string fid = g->folderId();
        std::lock_guard lock(mu_);
        if (!fid.empty() && fid != s_.gFolderId) {
            s_.gFolderId = fid;
            saveSettings(cfgPath_, s_);
        }
    }
}

void CloudService::runLogin() {
    const CloudSettings s = settings();
    if (s.gClientId.empty()) return setAuth("error", "Informe o Client ID do Google antes de conectar");
    auto sec = d_.secrets->get(kSecGClientSecret);
    GoogleOAuthFlow flow(GoogleOAuthConfig{s.gClientId, sec ? *sec : std::string()}, d_.random);
    auto listener = makeLoopbackListener();
    const int port = listener->start();
    if (port <= 0) return setAuth("error", "Não foi possível abrir a porta local para receber a autorização");
    const std::string url = flow.authUrl(port);
    setAuth("waiting", "Aguardando a autorização no navegador (até 3 minutos)…");
    if (d_.openBrowser) d_.openBrowser(url);
    std::string line;
    GoogleOAuthFlow::Redirect red;
    const auto cancelled = [this] { return stopping_.load(); };
    for (int guard = 0; guard < 10; ++guard) {
        if (!listener->waitRequestLine(line, 180000, cancelled)) return setAuth("error", "Tempo esgotado ou cancelado: a autorização não foi concluída");
        red = flow.parseRedirect(line);
        if (red.ok) break;
        listener->respond(400, kLoginFail);
        if (red.error != "Pedido de retorno inesperado") return setAuth("error", red.error);  // favicon etc.: keep waiting
    }
    if (!red.ok) return setAuth("error", red.error.empty() ? "Autorização não concluída" : red.error);
    listener->respond(200, kLoginPage);
    listener->close();
    auto r = d_.http->send(flow.tokenRequest(red.code));
    if (!r.ok()) return setAuth("error", r.error().message);
    auto ts = parseTokenResponse(r.value());
    if (!ts.ok()) return setAuth("error", ts.error().message);
    if (ts.value().refreshToken.empty())
        return setAuth("error", "O Google não devolveu um token de renovação: revogue o acesso do app em myaccount.google.com e tente de novo");
    d_.secrets->put(kSecGRefresh, ts.value().refreshToken);
    refreshHints();
    applySettings(true);
    setAuth("ok", "Conta do Google conectada");
}

std::string CloudService::handle(const std::vector<std::string>& a) {
    try {
        if (a.empty()) return "";
        const std::string& c = a[0];
        if (c == "cloudsave" && a.size() >= 2) {
            Json r;
            if (!hb::core::json::parse(a[1], r) || !r.isObj()) return "Configuração de backup inválida.";
            CloudSettings s = settings();
            if (!settingsFromJson(a[1], s)) return "Configuração de backup inválida.";
            auto secret = [&](const char* field, const char* name) {
                if (auto* v = r.get(field); v && v->isStr() && !v->s.empty()) {
                    std::string val;
                    for (unsigned char ch : v->s) if (ch >= 0x20 && ch != 0x7F) val += static_cast<char>(ch);
                    d_.secrets->put(name, val.substr(0, 512));
                }
            };
            secret("s3AccessKey", kSecS3Access);   // blank = keep the stored one (never echoed back)
            secret("s3SecretKey", kSecS3Secret);
            secret("gClientSecret", kSecGClientSecret);
            const CloudSettings old = settings();
            if (s.gClientId != old.gClientId || s.gFolderName != old.gFolderName) s.gFolderId.clear();
            {
                std::lock_guard lock(mu_);
                s_ = s;
                saveSettings(cfgPath_, s_);
            }
            refreshHints();
            applySettings(true);
            return s.enabled ? "Backup em nuvem salvo e ativo." : "Backup em nuvem salvo (desligado).";
        }
        if (c == "cloudclear" && a.size() >= 2) {
            if (a[1] == "s3") { d_.secrets->erase(kSecS3Access); d_.secrets->erase(kSecS3Secret); }
            else if (a[1] == "gdrive") {
                d_.secrets->erase(kSecGClientSecret);
                d_.secrets->erase(kSecGRefresh);
                std::lock_guard lock(mu_);
                s_.gAccount.clear();
                s_.gFolderId.clear();
                saveSettings(cfgPath_, s_);
            } else return "";
            refreshHints();
            applySettings(true);
            return "Credenciais removidas.";
        }
        if (c == "cloudtest") {
            if (asyncBusy_) return "Já existe uma operação em andamento.";
            startAsync([this] { runTest(); });
            return "";
        }
        if (c == "cloudgoogle" && a.size() >= 2) {
            if (a[1] == "login") {
                if (asyncBusy_) return "Já existe uma operação em andamento.";
                startAsync([this] { runLogin(); });
            }
            return "";
        }
        if (c == "cloudretry" && a.size() >= 2) {
            if (a[1] == "all") return std::to_string(engine_->retryAll()) + " envio(s) recolocado(s) na fila.";
            engine_->retry(a[1]);
            return "";
        }
        if (c == "cloudcancel" && a.size() >= 2) { engine_->cancel(a[1]); return ""; }
        if (c == "cloudclearfinished") { engine_->clearFinished(); return ""; }
        if (c == "cloudscan") {
            std::lock_guard lock(watchMu_);
            watch_->poll();
            return "";
        }
    } catch (...) {
    }
    return "";
}

std::string CloudService::stateJson() {
    const CloudSettings s = settings();
    const Totals t = engine_->totals();
    auto jobs = engine_->jobs();
    std::ostringstream o;
    std::lock_guard lock(mu_);
    o << "{\"t\":\"cloud_state\",\"status\":\"" << esc(status_) << "\",\"cfg\":{"
      << "\"enabled\":" << b(s.enabled) << ",\"provider\":\"" << esc(s.provider) << "\",\"s3Endpoint\":\"" << esc(s.s3Endpoint)
      << "\",\"s3Region\":\"" << esc(s.s3Region) << "\",\"s3Bucket\":\"" << esc(s.s3Bucket) << "\",\"s3Prefix\":\"" << esc(s.s3Prefix)
      << "\",\"s3Addressing\":\"" << esc(s.s3Addressing) << "\",\"partSizeMiB\":" << s.partSizeMiB << ",\"gClientId\":\""
      << esc(s.gClientId) << "\",\"gFolderName\":\"" << esc(s.gFolderName) << "\",\"gAccount\":\"" << esc(s.gAccount)
      << "\",\"bandwidthMbps\":" << s.bandwidthMbps << ",\"pauseWhileStreaming\":" << b(s.pauseWhileStreaming)
      << ",\"upProgram\":" << b(s.upProgram) << ",\"upIsoVideo\":" << b(s.upIsoVideo) << ",\"upIsoAudio\":" << b(s.upIsoAudio)
      << ",\"deleteAfterUpload\":" << b(s.deleteAfterUpload) << ",\"watchFolder\":" << b(s.watchFolder)
      << ",\"stableSeconds\":" << s.stableSeconds << "},\"secrets\":{\"s3Access\":\"" << esc(hintS3Access_) << "\",\"s3Secret\":\""
      << esc(hintS3Secret_) << "\",\"gClientSecret\":\"" << esc(hintGClient_) << "\",\"gConnected\":" << b(!hintGRefresh_.empty())
      << "},\"test\":{\"state\":\"" << testState_ << "\",\"msg\":\"" << esc(testMsg_) << "\"},\"auth\":{\"state\":\"" << authState_
      << "\",\"msg\":\"" << esc(authMsg_) << "\"},\"totals\":{\"queued\":" << t.queued << ",\"uploading\":" << t.uploading
      << ",\"done\":" << t.done << ",\"error\":" << t.error << ",\"bytesTotal\":" << t.bytesTotal << ",\"bytesDone\":" << t.bytesDone
      << ",\"bps\":" << static_cast<long long>(t.bytesPerSec) << ",\"paused\":" << b(t.paused) << ",\"pauseReason\":\""
      << esc(t.pauseReason) << "\"},\"jobs\":[";
    bool first = true;
    int shown = 0;
    for (auto it = jobs.rbegin(); it != jobs.rend() && shown < 60; ++it) {
        if (it->state == JobState::Cancelled) continue;
        o << (first ? "" : ",") << "{\"id\":\"" << esc(it->id) << "\",\"name\":\"" << esc(baseName(it->path)) << "\",\"kind\":\""
          << esc(it->kind) << "\",\"state\":\"" << jobStateName(it->state) << "\",\"size\":" << it->size << ",\"done\":" << it->done
          << ",\"attempts\":" << it->attempts << ",\"error\":\"" << esc(it->error) << "\",\"note\":\"" << esc(it->note) << "\"}";
        first = false;
        ++shown;
    }
    o << "]}";
    return o.str();
}

}  // namespace hb::cloud
