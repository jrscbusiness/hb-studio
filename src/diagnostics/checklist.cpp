#include "diagnostics/checklist.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace hb::diag {

namespace {

std::string fmt1(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.1f", v);
    for (char* p = b; *p; ++p)
        if (*p == '.') *p = ',';   // pt-BR decimal comma
    return b;
}

std::string fmt0(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

std::string gb(std::int64_t bytes) { return fmt1(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) + " GB"; }

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

CheckResult make(const char* id, const char* title) {
    CheckResult r;
    r.id = id;
    r.title = title;
    return r;
}

std::string joinNames(const std::vector<std::string>& v, std::size_t maxShown = 4) {
    std::string o;
    for (std::size_t i = 0; i < v.size() && i < maxShown; ++i) o += (i ? ", " : "") + v[i];
    if (v.size() > maxShown) o += " e mais " + std::to_string(v.size() - maxShown);
    return o;
}

int neededKbps(const ChecklistInput& in) {
    long long sum = 0;
    for (const auto& d : in.destinations)
        if (d.enabled) sum += static_cast<long long>(d.videoKbps) + d.audioKbps;
    return static_cast<int>(sum > 2000000000LL ? 2000000000LL : sum);
}

}  // namespace

const char* checkStatusName(CheckStatus s) {
    switch (s) {
        case CheckStatus::Ok: return "ok";
        case CheckStatus::Warn: return "warn";
        case CheckStatus::Fail: return "fail";
        case CheckStatus::Skip: return "skip";
    }
    return "skip";
}

bool isValidSpeedTestUrl(const std::string& url) {
    static const std::string kScheme = "https://";
    if (url.size() <= kScheme.size() || url.size() > kUploadUrlMaxLen) return false;
    for (std::size_t i = 0; i < kScheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(url[i])) != kScheme[i]) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7f || c == '\\') return false;
    std::size_t e = kScheme.size();
    while (e < url.size() && url[e] != '/' && url[e] != '?' && url[e] != '#') ++e;
    const std::string auth = url.substr(kScheme.size(), e - kScheme.size());
    if (auth.empty() || auth.find('@') != std::string::npos || auth[0] == ':') return false;
    return true;
}

CheckResult checkDisk(const ChecklistInput& in, const Probes& p) {
    CheckResult r = make("disk", "Espaço em disco para gravar");
    if (in.recordingFolder.empty()) {
        r.detail = "Pasta de gravação não definida.";
        r.hint = "Escolha a pasta na aba GRAVAÇÃO.";
        r.status = CheckStatus::Warn;
        return r;
    }
    const std::int64_t free = p.freeDiskBytes ? p.freeDiskBytes(in.recordingFolder) : -1;
    if (free < 0) {
        r.detail = "Não foi possível medir o espaço livre.";
        r.status = CheckStatus::Skip;
        return r;
    }
    const long long kbps = static_cast<long long>(in.recordingVideoKbps) + in.recordingAudioKbps;
    r.detail = gb(free) + " livres";
    double minutes = -1;
    if (kbps > 0) {
        minutes = static_cast<double>(free) * 8.0 / (static_cast<double>(kbps) * 1000.0) / 60.0;
        r.detail += " (~" + fmt0(std::floor(minutes)) + " min de gravação a " + std::to_string(kbps) + " kbps)";
    }
    if (free <= in.minFreeBytes) {
        r.status = CheckStatus::Fail;
        r.hint = "Libere espaço: a gravação para abaixo do limite mínimo configurado.";
    } else if (minutes >= 0 && minutes < kDiskWarnMinutes) {
        r.status = CheckStatus::Fail;
        r.hint = "Libere espaço ou escolha outra pasta/disco antes de entrar ao vivo.";
    } else if (minutes >= 0 && minutes < kDiskOkMinutes) {
        r.status = CheckStatus::Warn;
        r.hint = "Pode não bastar para um culto longo: libere espaço ou reduza o bitrate.";
    } else {
        r.status = CheckStatus::Ok;
    }
    return r;
}

CheckResult checkNetwork(const NetProbeResult& net, const Probes& p) {
    CheckResult r = make("net", "Internet");
    if (!p.reachability) {
        r.detail = "Verificação indisponível nesta plataforma.";
        r.status = CheckStatus::Skip;
        return r;
    }
    if (net.reachable) {
        r.status = CheckStatus::Ok;
        r.detail = "Conexão HTTPS funcionando.";
    } else {
        r.status = CheckStatus::Fail;
        r.detail = "Sem acesso à internet" + (net.error.empty() ? std::string() : " (" + net.error + ")") + ".";
        r.hint = "Verifique o cabo/Wi-Fi, o roteador e se um firewall ou proxy bloqueia o HB Studio.";
    }
    return r;
}

CheckResult checkUpload(const ChecklistInput& in, const NetProbeResult& net, const Probes& p) {
    CheckResult r = make("upload", "Velocidade de envio (upload)");
    const int need = neededKbps(in);
    if (need <= 0) {
        r.detail = "Nenhum destino de transmissão ligado.";
        r.status = CheckStatus::Skip;
        return r;
    }
    const std::string needText = "Necessário: " + std::to_string(need) + " kbps (vídeo+áudio dos destinos ligados). ";
    if (!in.speedTestEnabled) {
        r.detail = needText + "Teste de upload desligado (privacidade).";
        r.hint = "Para medir, ative o teste e informe um endereço HTTPS que aceite envios.";
        r.status = CheckStatus::Skip;
        return r;
    }
    if (!isValidSpeedTestUrl(in.speedTestUrl)) {
        r.detail = needText + "Endereço do teste inválido.";
        r.hint = "Use um endereço https:// completo, sem usuário/senha.";
        r.status = CheckStatus::Warn;
        return r;
    }
    if (!p.uploadTest) {
        r.detail = needText + "Teste indisponível nesta plataforma.";
        r.status = CheckStatus::Skip;
        return r;
    }
    if (p.reachability && !net.reachable) {
        r.detail = needText + "Sem internet para testar.";
        r.status = CheckStatus::Skip;
        return r;
    }
    const UploadProbeResult up = p.uploadTest(in.speedTestUrl);
    if (!up.ok) {
        r.detail = needText + "O teste falhou" + (up.error.empty() ? std::string() : " (" + up.error + ")") + ".";
        r.hint = "Confira o endereço do teste e a conexão; tente de novo.";
        r.status = CheckStatus::Warn;
        return r;
    }
    r.detail = needText + "Medido: ~" + fmt0(up.kbps) + " kbps (estimativa curta, pode variar).";
    if (up.kbps < need) {
        r.status = CheckStatus::Fail;
        r.hint = "Reduza o bitrate dos destinos ou use uma conexão mais rápida/cabo.";
    } else if (up.kbps < need * kUploadHeadroom) {
        r.status = CheckStatus::Warn;
        r.hint = "Folga pequena: considere reduzir o bitrate para evitar quedas.";
    } else {
        r.status = CheckStatus::Ok;
    }
    return r;
}

CheckResult checkCameras(const ChecklistInput& in) {
    CheckResult r = make("cameras", "Câmeras");
    std::vector<std::string> offline;
    int active = 0, online = 0, disabled = 0;
    for (const auto& c : in.cameras) {
        if (!c.enabled) { ++disabled; continue; }
        ++active;
        if (c.online) ++online;
        else offline.push_back("CAM " + std::to_string(c.slot + 1) + (c.name.empty() ? std::string() : " " + c.name));
    }
    if (active == 0) {
        r.status = CheckStatus::Warn;
        r.detail = "Nenhuma câmera ligada/atribuída" + (disabled ? " (" + std::to_string(disabled) + " desligada(s))" : std::string()) + ".";
        r.hint = "Atribua uma câmera na aba CÂMERAS (ou ignore se só usar mídia/cenas).";
        return r;
    }
    r.detail = std::to_string(online) + " de " + std::to_string(active) + " câmeras online.";
    if (online == active) {
        r.status = CheckStatus::Ok;
    } else {
        r.status = online == 0 ? CheckStatus::Fail : CheckStatus::Warn;
        r.detail += " Offline: " + joinNames(offline) + ".";
        r.hint = "Confira cabo/USB/energia e clique em atualizar dispositivos na aba CÂMERAS.";
    }
    return r;
}

CheckResult checkAudio(const ChecklistInput& in) {
    CheckResult r = make("audio", "Áudio");
    std::vector<std::string> missing, silent;
    int assigned = 0;
    for (const auto& s : in.audio) {
        if (!s.assigned) continue;
        ++assigned;
        const std::string label = s.name.empty() ? "canal " + std::to_string(s.index + 1) : s.name;
        if (s.state == "error" || s.state == "off") missing.push_back(label);
        else if (s.state == "nosignal" || s.peakDb < kSilenceDb) silent.push_back(label);
    }
    if (assigned == 0) {
        r.status = CheckStatus::Warn;
        r.detail = "Nenhum canal de áudio com dispositivo atribuído.";
        r.hint = "Atribua um microfone/entrada na aba ÁUDIO.";
        return r;
    }
    if (!missing.empty()) {
        r.status = CheckStatus::Fail;
        r.detail = "Dispositivo ausente ou com erro: " + joinNames(missing) + ".";
        r.hint = "Reconecte o dispositivo e reatribua o canal na aba ÁUDIO.";
    } else if (!silent.empty()) {
        r.status = CheckStatus::Warn;
        r.detail = "Sem sinal agora: " + joinNames(silent) + " (" + std::to_string(assigned - static_cast<int>(silent.size())) + " de " +
                   std::to_string(assigned) + " com sinal).";
        r.hint = "Fale/toque no microfone e confira mute, ganho e a chave física do equipamento.";
    } else {
        r.status = CheckStatus::Ok;
        r.detail = std::to_string(assigned) + " canal(is) com sinal.";
    }
    return r;
}

CheckResult checkEncoder(const Probes& p) {
    CheckResult r = make("encoder", "Codificador H.264");
    const EncoderProbeResult e = p.encoders ? p.encoders() : EncoderProbeResult{};
    if (!e.known) {
        r.status = CheckStatus::Skip;
        r.detail = "Não foi possível enumerar os codificadores (não verificado).";
        return r;
    }
    if (e.hardwareH264) {
        r.status = CheckStatus::Ok;
        r.detail = "Codificador por hardware disponível.";
    } else if (e.softwareH264) {
        r.status = CheckStatus::Warn;
        r.detail = "Só codificação por software (usa mais CPU).";
        r.hint = "Reduza resolução/bitrate/FPS se o computador engasgar; atualize o driver da placa de vídeo.";
    } else {
        r.status = CheckStatus::Fail;
        r.detail = "Nenhum codificador H.264 encontrado.";
        r.hint = "Instale o Media Feature Pack (edições N do Windows) ou atualize o driver da placa de vídeo.";
    }
    if (!e.names.empty()) r.detail += " [" + e.names + "]";
    return r;
}

CheckResult checkDestinations(const ChecklistInput& in) {
    CheckResult r = make("destinations", "Destinos de transmissão");
    std::vector<std::string> bad;
    int enabled = 0;
    for (const auto& d : in.destinations) {
        if (!d.enabled) continue;
        ++enabled;
        if (!d.urlSet || !d.keySet) bad.push_back(d.name + (!d.urlSet && !d.keySet ? " (sem URL e sem chave)" : !d.urlSet ? " (sem URL)" : " (sem chave)"));
    }
    if (enabled == 0) {
        r.status = CheckStatus::Warn;
        r.detail = "Nenhum destino ligado.";
        r.hint = "Ligue um destino na aba TRANSMISSÃO (ignore se for só gravar).";
    } else if (!bad.empty()) {
        r.status = CheckStatus::Fail;
        r.detail = "Incompleto: " + joinNames(bad) + ".";
        r.hint = "Preencha o servidor e a chave de transmissão do destino na aba TRANSMISSÃO.";
    } else {
        r.status = CheckStatus::Ok;
        r.detail = std::to_string(enabled) + " destino(s) com servidor e chave definidos.";
    }
    return r;
}

CheckResult checkClock(const NetProbeResult& net, const Probes& p) {
    CheckResult r = make("clock", "Relógio do computador");
    if (!p.nowEpochSec) {
        r.status = CheckStatus::Skip;
        r.detail = "Não verificado.";
        return r;
    }
    const std::int64_t now = p.nowEpochSec();
    if (now < kClockMinPlausibleEpoch) {
        r.status = CheckStatus::Fail;
        r.detail = "A data do computador parece errada.";
        r.hint = "Ajuste data/hora do Windows (sincronização automática): plataformas rejeitam conexões com relógio errado.";
        return r;
    }
    if (net.serverEpochSec > 0) {
        const std::int64_t skew = now > net.serverEpochSec ? now - net.serverEpochSec : net.serverEpochSec - now;
        r.detail = "Diferença para a hora da internet: " + std::to_string(skew) + " s.";
        if (skew > 600) {
            r.status = CheckStatus::Fail;
            r.hint = "Sincronize o relógio do Windows (Configurações > Hora e idioma > Sincronizar agora).";
        } else if (skew > kClockMaxSkewSec) {
            r.status = CheckStatus::Warn;
            r.hint = "Sincronize o relógio do Windows para evitar falhas de login/transmissão.";
        } else {
            r.status = CheckStatus::Ok;
        }
    } else {
        r.status = CheckStatus::Ok;
        r.detail = "Data plausível (não comparada com a internet).";
    }
    return r;
}

std::vector<CheckResult> runChecklist(const ChecklistInput& in, const Probes& p) {
    std::vector<CheckResult> out;
    NetProbeResult net;
    try {
        if (p.reachability) net = p.reachability();
    } catch (...) {
        net = NetProbeResult{};
        net.error = "erro interno";
    }
    // Each check is isolated: a throwing probe turns that line into a Skip, never aborts the checklist.
    auto guarded = [&](const char* id, const char* title, auto&& fn) {
        try {
            out.push_back(fn());
        } catch (...) {
            CheckResult r = make(id, title);
            r.status = CheckStatus::Skip;
            r.detail = "Verificação falhou internamente.";
            out.push_back(std::move(r));
        }
    };
    guarded("disk", "Espaço em disco para gravar", [&] { return checkDisk(in, p); });
    guarded("net", "Internet", [&] { return checkNetwork(net, p); });
    guarded("upload", "Velocidade de envio (upload)", [&] { return checkUpload(in, net, p); });
    guarded("destinations", "Destinos de transmissão", [&] { return checkDestinations(in); });
    guarded("cameras", "Câmeras", [&] { return checkCameras(in); });
    guarded("audio", "Áudio", [&] { return checkAudio(in); });
    guarded("encoder", "Codificador H.264", [&] { return checkEncoder(p); });
    guarded("clock", "Relógio do computador", [&] { return checkClock(net, p); });
    return out;
}

std::string checklistJson(const std::vector<CheckResult>& r) {
    int ok = 0, warn = 0, fail = 0;
    std::string items;
    for (const auto& c : r) {
        if (c.status == CheckStatus::Ok) ++ok;
        else if (c.status == CheckStatus::Warn) ++warn;
        else if (c.status == CheckStatus::Fail) ++fail;
        if (!items.empty()) items += ',';
        items += "{\"id\":\"" + jsonEsc(c.id) + "\",\"s\":\"" + checkStatusName(c.status) + "\",\"title\":\"" + jsonEsc(c.title) +
                 "\",\"detail\":\"" + jsonEsc(c.detail) + "\",\"hint\":\"" + jsonEsc(c.hint) + "\"}";
    }
    return "{\"t\":\"checklist\",\"ok\":" + std::to_string(ok) + ",\"warn\":" + std::to_string(warn) + ",\"fail\":" + std::to_string(fail) +
           ",\"results\":[" + items + "]}";
}

}  // namespace hb::diag
