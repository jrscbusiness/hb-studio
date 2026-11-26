#include "diagnostics/report.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

namespace hb::diag {

namespace {

std::string clockText(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%H:%M:%S", &tm);
    return b;
}

std::string isoText(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[40];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return b;
}

std::string oneLine(std::string s) {
    for (auto& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

std::string num1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

void section(std::string& o, const char* title) {
    o += "\n== ";
    o += title;
    o += " ==\n";
}

}  // namespace

std::vector<ModuleError> lastErrorsPerModule(const std::vector<core::LogRecord>& log) {
    std::vector<ModuleError> out;
    std::set<std::string> seen;
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (it->level < core::Level::Warning) continue;
        if (!seen.insert(it->module).second) continue;
        out.push_back({it->module, std::string(core::levelName(it->level)) + ": " + it->message, clockText(it->time)});
    }
    return out;
}

std::string buildReport(const DiagnosticsInput& in, const Redactor& redactor, std::chrono::system_clock::time_point now) {
    std::string o;
    o += "HB Studio - diagnóstico\n";
    o += "Gerado em: " + isoText(now) + "\n";
    o += "Este arquivo NÃO contém chaves, tokens, senhas, IPs nem o nome de usuário do Windows (revise antes de enviar).\n";

    section(o, "Aplicativo");
    o += "Versão: " + in.appVersion + "\n";
    o += "Qualidade do preview (efetiva): " + in.previewQuality + "\n";
    o += "Formato de saída (efetivo): " + in.outputFormat + "\n";

    section(o, "Sistema");
    o += "Windows: " + in.windowsVersion + "\n";
    o += "CPU: " + in.cpuName + " (" + std::to_string(in.logicalCores) + " núcleos lógicos)\n";
    o += "RAM: " + num1(in.ramGiB) + " GiB\n";
    for (const auto& g : in.gpus) o += "GPU: " + g + "\n";
    if (in.gpus.empty()) o += "GPU: (não detectada)\n";
    for (std::size_t i = 0; i < in.monitors.size(); ++i) o += "Monitor " + std::to_string(i + 1) + ": " + in.monitors[i] + "\n";

    section(o, "Dispositivos de vídeo");
    for (const auto& d : in.videoDevices) o += "- " + d.name + " [" + d.status + "]" + (d.detail.empty() ? "" : " " + d.detail) + "\n";
    if (in.videoDevices.empty()) o += "(nenhum)\n";

    section(o, "Dispositivos de áudio");
    for (const auto& d : in.audioDevices) o += "- " + d.name + " [" + d.status + "]" + (d.detail.empty() ? "" : " " + d.detail) + "\n";
    if (in.audioDevices.empty()) o += "(nenhum)\n";

    section(o, "Recursos");
    for (const auto& f : in.features) o += "- " + f.key + ": " + f.value + "\n";

    section(o, "Configurações (sem segredos)");
    for (const auto& s : in.settings) {
        o += "- " + s.key + ": ";
        o += s.secret ? (s.value.empty() ? "(vazio)" : "(definido)") : oneLine(s.value);
        o += "\n";
    }

    section(o, "Destinos de transmissão");
    for (const auto& d : in.destinations) {
        o += "- " + d.name + " (" + d.platform + "/" + d.protocol + ") " + (d.enabled ? "ligado" : "desligado");
        const std::string host = urlSchemeHost(d.serverUrl);
        o += "; servidor: " + (host.empty() ? std::string(d.urlSet ? "(formato inválido)" : "(vazio)") : host);
        o += "; chave: " + std::string(d.keySet ? "(definida)" : "(vazia)");
        o += "; " + std::to_string(d.videoKbps) + "+" + std::to_string(d.audioKbps) + " kbps";
        if (!d.status.empty()) o += "; estado: " + oneLine(d.status);
        o += "\n";
    }
    if (in.destinations.empty()) o += "(nenhum)\n";

    section(o, "Últimos erros por módulo");
    std::vector<ModuleError> errs = in.extraErrors;
    for (auto& e : lastErrorsPerModule(in.log)) errs.push_back(std::move(e));
    for (const auto& e : errs)
        o += "- " + e.module + (e.when.empty() ? "" : " (" + e.when + ")") + ": " + oneLine(e.message) + "\n";
    if (errs.empty()) o += "(nenhum)\n";

    section(o, "Registro (últimas linhas, memória)");
    const std::size_t total = in.log.size();
    const std::size_t from = total > in.logTailLines ? total - in.logTailLines : 0;
    for (std::size_t i = from; i < total; ++i) o += core::formatRecord(in.log[i]) + "\n";
    if (total == 0) o += "(vazio)\n";

    return redactor.redact(o);
}

bool writeTextFile(const std::string& path, const std::string& text, std::string& error) {
    if (path.empty()) { error = "caminho vazio"; return false; }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "não foi possível criar o arquivo"; return false; }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.flush();
    if (!f) { error = "falha ao gravar o arquivo"; return false; }
    return true;
}

}  // namespace hb::diag
