#include "captions/controller.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>

#include "captions/srt.hpp"
#include "core/json.hpp"

namespace hb::captions {
namespace fs = std::filesystem;
namespace {

std::string esc(const std::string& s) { return hb::core::json::escape(s); }

std::string defaultTimestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tmv);
    return buf;
}

std::string toUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}
fs::path fromUtf8(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

}  // namespace

CaptionController::CaptionController(ControllerDeps deps)
    : d_(std::move(deps)), models_(d_.modelsDir, d_.downloader, d_.catalog.empty() ? modelCatalog() : d_.catalog) {
    loadSettings(d_.settingsPath, settings_);
    if (!d_.timestamp) d_.timestamp = defaultTimestamp;
    if (d_.bus) {
        capSub_ = d_.bus->subscribe("caption.text", [this](const hb::core::Event& e) { onCaption(e); });
        fileSub_ = d_.bus->subscribe("recording.fileClosed", [this](const hb::core::Event& e) { onFileClosed(e); });
    }
}

CaptionController::~CaptionController() { shutdown(); }

void CaptionController::shutdown() {
    if (d_.bus) {
        if (capSub_) d_.bus->unsubscribe(capSub_);
        if (fileSub_) d_.bus->unsubscribe(fileSub_);
        capSub_ = fileSub_ = 0;
    }
    dlCancel_ = true;
    if (dlThread_.joinable()) dlThread_.join();
    std::lock_guard lock(ctlMu_);
    stopLocked();
}

void CaptionController::onCaption(const hb::core::Event& e) {
    const auto t = e.fields.find("text");
    const auto f = e.fields.find("final");
    if (t == e.fields.end() || f == e.fields.end()) return;
    std::lock_guard lock(stateMu_);
    if (!running_) return;   // ignore captions from other publishers when ours is off
    if (f->second == "1") {
        if (!t->second.empty()) {
            std::string line = t->second;
            std::replace(line.begin(), line.end(), '\n', ' ');
            lines_.push_back(std::move(line));
            while (lines_.size() > 8) lines_.pop_front();
        }
        partial_.clear();
    } else {
        partial_ = t->second;
    }
}

void CaptionController::onFileClosed(const hb::core::Event& e) {
    const auto k = e.fields.find("kind");
    const auto p = e.fields.find("path");
    if (k == e.fields.end() || p == e.fields.end() || k->second != "program" || p->second.empty()) return;
    std::shared_ptr<CaptionService> svc;
    bool save = false;
    std::string srtPath;
    try {
        fs::path q = fromUtf8(p->second);
        q.replace_extension(".srt");
        srtPath = toUtf8(q);
    } catch (...) {
        return;
    }
    {
        std::lock_guard lock(stateMu_);
        if (!running_ || !service_) return;
        svc = service_;
        save = settings_.saveSrt;
        sessionSrtPath_ = srtPath;
    }
    if (save && svc->srtCueCount() > 0) writeSrt(srtPath, svc->srtText());
}

void CaptionController::writeSrt(const std::string& path, const std::string& text) {
    const bool ok = saveSrtFile(path, text);
    std::lock_guard lock(stateMu_);
    if (ok) srtFile_ = path;
    else message_ = "Não foi possível salvar o arquivo .srt.";
}

std::string CaptionController::fallbackSrtPath() {
    std::string folder;
    try {
        if (d_.srtFolder) folder = d_.srtFolder();
    } catch (...) {
    }
    if (folder.empty()) return {};
    try {
        return toUtf8(fromUtf8(folder) / fromUtf8("HBStudio_legendas_" + d_.timestamp() + ".srt"));
    } catch (...) {
        return {};
    }
}

bool CaptionController::startLocked() {
    if (running_) return true;
    CaptionSettings s;
    {
        std::lock_guard lock(stateMu_);
        s = settings_;
        message_.clear();
        lines_.clear();
        partial_.clear();
        sessionSrtPath_.clear();
        srtFile_.clear();
    }
    auto fail = [&](const std::string& why) {
        std::lock_guard lock(stateMu_);
        message_ = why;
        return false;
    };
    std::unique_ptr<ISpeechRecognizer> rec;
    if (s.engine == "cloud") {
        CloudConfig cc;
        cc.baseUrl = s.cloudBaseUrl;
        cc.apiKey = s.cloudKey;
        cc.model = s.cloudModel;
        const std::string why = validateBaseUrl(cc.baseUrl);
        if (!why.empty()) return fail("Nuvem: " + why + ".");
        if (!d_.http) return fail("Esta compilação não tem cliente HTTP.");
        rec = std::make_unique<CloudRecognizer>(std::move(cc), d_.http);
    } else {
        if (!d_.makeWhisper) return fail("Esta compilação não inclui o motor local (whisper.cpp).");
        if (!models_.isInstalled(s.model)) return fail("Baixe o modelo de voz antes de ligar as legendas.");
        rec = d_.makeWhisper(models_.pathFor(s.model));
        if (!rec) return fail("Não foi possível criar o reconhecedor local.");
    }
    if (!d_.addProgramConsumer) return fail("Áudio do Program indisponível.");
    SessionConfig sc;
    sc.language = s.language;
    sc.normalize.profanityFilter = s.profanityFilter;
    sc.normalize.maxCharsPerLine = static_cast<std::size_t>(s.maxCharsPerLine);
    sc.normalize.maxLines = static_cast<std::size_t>(s.maxLines);
    auto svc = std::make_shared<CaptionService>(std::move(rec), d_.bus, sc);
    {
        std::lock_guard lock(stateMu_);
        service_ = svc;
        running_ = true;
    }
    CaptionService* raw = svc.get();
    consumerId_ = d_.addProgramConsumer([raw](const float* p, std::size_t n) { raw->pushProgramAudio(p, n); });
    svc->start();
    return true;
}

void CaptionController::stopLocked() {
    std::shared_ptr<CaptionService> svc;
    {
        std::lock_guard lock(stateMu_);
        svc = service_;
    }
    if (!svc) return;
    if (consumerId_ && d_.removeProgramConsumer) d_.removeProgramConsumer(consumerId_);   // synchronous: no more pushes
    consumerId_ = 0;
    svc->stop();   // joins the worker (it may be publishing: stateMu_ must not be held here)
    const ServiceStatus st = svc->status();
    const std::string srt = svc->srtText();
    const std::size_t cues = svc->srtCueCount();
    bool save;
    std::string path;
    {
        std::lock_guard lock(stateMu_);
        running_ = false;
        save = settings_.saveSrt;
        path = sessionSrtPath_;
    }
    if (save && cues > 0) {
        if (path.empty()) path = fallbackSrtPath();
        if (!path.empty()) writeSrt(path, srt);
    }
    std::lock_guard lock(stateMu_);
    if (st.state == ServiceStatus::State::Error) message_ = st.error;
    partial_.clear();
    service_.reset();
}

bool CaptionController::handleCommand(const std::string& msg) {
    if (msg.rfind("cap|", 0) != 0) return false;
    const std::string rest = msg.substr(4);
    const std::size_t bar = rest.find('|');
    const std::string cmd = rest.substr(0, bar);
    const std::string arg = bar == std::string::npos ? std::string() : rest.substr(bar + 1);
    try {
        if (cmd == "on") {
            std::lock_guard lock(ctlMu_);
            startLocked();
        } else if (cmd == "off") {
            std::lock_guard lock(ctlMu_);
            stopLocked();
        } else if (cmd == "set") {
            const std::size_t b2 = arg.find('|');
            if (b2 == std::string::npos) return true;
            const std::string key = arg.substr(0, b2), value = arg.substr(b2 + 1);
            CaptionSettings copy;
            {
                std::lock_guard lock(stateMu_);
                if (!applySetting(settings_, key, value)) { message_ = "Valor inválido para \"" + key + "\"."; return true; }
                message_.clear();
                copy = settings_;
            }
            saveSettings(d_.settingsPath, copy);   // takes effect the next time the captions are switched on
        } else if (cmd == "dl") {
            startDownload(arg);
        } else if (cmd == "dlcancel") {
            dlCancel_ = true;
        } else if (cmd == "clear") {
            std::lock_guard lock(stateMu_);
            lines_.clear();
            partial_.clear();
        }
    } catch (...) {
        std::lock_guard lock(stateMu_);
        message_ = "Erro interno nas legendas.";
    }
    return true;
}

void CaptionController::startDownload(const std::string& id) {
    {
        std::lock_guard lock(stateMu_);
        if (dlActive_) return;
        if (!findModel(models_.catalog(), id)) { message_ = "Modelo desconhecido."; return; }
        dlActive_ = true;
        dlId_ = id;
        dlGot_ = dlTotal_ = 0;
        dlError_.clear();
        message_.clear();
    }
    if (dlThread_.joinable()) dlThread_.join();   // a finished previous download
    dlCancel_ = false;
    dlThread_ = std::thread([this, id] {
        std::string err;
        const bool ok = models_.ensure(
            id,
            [this](std::uint64_t got, std::uint64_t total) {
                std::lock_guard lock(stateMu_);
                dlGot_ = got;
                dlTotal_ = total;
                return !dlCancel_.load();
            },
            err);
        std::lock_guard lock(stateMu_);
        dlActive_ = false;
        dlError_ = ok || err == "cancelado" ? std::string() : err;
    });
}

std::string CaptionController::stateJson() {
    CaptionSettings s;
    std::string message, partial, srtFile, dlId, dlError;
    std::deque<std::string> lines;
    std::shared_ptr<CaptionService> svc;
    bool dlActive;
    std::uint64_t got, total;
    {
        std::lock_guard lock(stateMu_);
        s = settings_;
        message = message_;
        partial = partial_;
        srtFile = srtFile_;
        lines = lines_;
        svc = service_;
        dlActive = dlActive_;
        dlId = dlId_;
        dlError = dlError_;
        got = dlGot_;
        total = dlTotal_;
    }
    ServiceStatus st;
    if (svc) st = svc->status();
    const bool on = running_.load();
    std::string j = "{\"t\":\"cap\",\"on\":" + std::string(on ? "true" : "false") + ",\"state\":\"" + stateName(st.state) + "\"";
    j += ",\"engine\":\"" + esc(st.engine) + "\",\"latencyMs\":" + std::to_string(st.latencyMs) + ",\"decodeMs\":" +
         std::to_string(st.decodeMs) + ",\"droppedMs\":" + std::to_string(st.droppedMs) + ",\"finals\":" + std::to_string(st.finals);
    j += ",\"error\":\"" + esc(on ? st.error : std::string()) + "\",\"msg\":\"" + esc(message) + "\"";
    j += ",\"partial\":\"" + esc(partial) + "\",\"lines\":[";
    for (std::size_t i = 0; i < lines.size(); ++i) j += (i ? ",\"" : "\"") + esc(lines[i]) + "\"";
    j += "],\"srtFile\":\"" + esc(srtFile) + "\",\"whisper\":" + (d_.makeWhisper ? "true" : "false");
    j += ",\"cfg\":{\"engine\":\"" + esc(s.engine) + "\",\"model\":\"" + esc(s.model) + "\",\"language\":\"" + esc(s.language) +
         "\",\"profanity\":" + (s.profanityFilter ? "true" : "false") + ",\"saveSrt\":" + (s.saveSrt ? "true" : "false") +
         ",\"maxChars\":" + std::to_string(s.maxCharsPerLine) + ",\"maxLines\":" + std::to_string(s.maxLines) +
         ",\"cloudUrl\":\"" + esc(s.cloudBaseUrl) + "\",\"cloudModel\":\"" + esc(s.cloudModel) + "\",\"hasKey\":" +
         (s.cloudKey.empty() ? "false" : "true") + "},\"models\":[";
    bool first = true;
    for (const auto& m : models_.catalog()) {
        j += std::string(first ? "" : ",") + "{\"id\":\"" + esc(m.id) + "\",\"label\":\"" + esc(m.label) + "\",\"note\":\"" +
             esc(m.note) + "\",\"sizeMb\":" + std::to_string(m.sizeBytes / 1000000) + ",\"ramMb\":" + std::to_string(m.approxRamMb) +
             ",\"installed\":" + (models_.isInstalled(m.id) ? "true" : "false") + "}";
        first = false;
    }
    const int pct = total ? static_cast<int>(std::min<std::uint64_t>(100, got * 100 / total)) : 0;
    j += "],\"dl\":{\"active\":" + std::string(dlActive ? "true" : "false") + ",\"id\":\"" + esc(dlId) + "\",\"pct\":" +
         std::to_string(pct) + ",\"error\":\"" + esc(dlError) + "\"}}";
    return j;
}

std::string CaptionController::pollJson(bool force) {
    std::string j = stateJson();
    std::lock_guard lock(stateMu_);
    if (!force && j == lastPosted_) return {};
    lastPosted_ = j;
    return j;
}

}  // namespace hb::captions
