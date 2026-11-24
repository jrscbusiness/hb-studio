#include "captions/cloud_recognizer.hpp"

#include <algorithm>
#include <cmath>

#include "core/json.hpp"

namespace hb::captions {
namespace {
void le16(std::string& s, std::uint16_t v) { s.push_back(static_cast<char>(v & 0xFF)); s.push_back(static_cast<char>(v >> 8)); }
void le32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }

bool isLoopbackHost(const std::string& hostPort) {
    std::string host = hostPort;
    if (!host.empty() && host[0] == '[') {
        const auto e = host.find(']');
        host = e == std::string::npos ? host : host.substr(0, e + 1);
    } else {
        const auto c = host.find(':');
        if (c != std::string::npos) host = host.substr(0, c);
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}
}  // namespace

std::string encodeWav16(const float* pcm, std::size_t samples, int sampleRate) {
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples * 2);
    std::string w;
    w.reserve(44 + dataBytes);
    w += "RIFF";
    le32(w, 36 + dataBytes);
    w += "WAVEfmt ";
    le32(w, 16);
    le16(w, 1);   // PCM
    le16(w, 1);   // mono
    le32(w, static_cast<std::uint32_t>(sampleRate));
    le32(w, static_cast<std::uint32_t>(sampleRate) * 2);
    le16(w, 2);
    le16(w, 16);
    w += "data";
    le32(w, dataBytes);
    for (std::size_t i = 0; i < samples; ++i) {
        const float x = std::isfinite(pcm[i]) ? std::clamp(pcm[i], -1.0f, 1.0f) : 0.0f;
        le16(w, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrintf(x * 32767.0f))));
    }
    return w;
}

std::string buildMultipart(const std::string& boundary, const std::vector<std::pair<std::string, std::string>>& fields,
                           const std::string& fileField, const std::string& filename, const std::string& fileContentType,
                           const std::string& fileData) {
    std::string b;
    for (const auto& f : fields)
        b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + f.first + "\"\r\n\r\n" + f.second + "\r\n";
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fileField + "\"; filename=\"" + filename +
         "\"\r\nContent-Type: " + fileContentType + "\r\n\r\n";
    b += fileData;
    b += "\r\n--" + boundary + "--\r\n";
    return b;
}

bool parseTranscriptionJson(const std::string& body, std::string& text) {
    hb::core::json::Value v;
    if (!hb::core::json::parse(body, v) || !v.isObj()) return false;
    const auto* t = v.get("text");
    if (!t || !t->isStr()) return false;
    text = t->s;
    return true;
}

std::string validateBaseUrl(const std::string& url) {
    if (url.size() > 512) return "endereço longo demais";
    for (unsigned char c : url)
        if (c < 0x21 || c == '"' || c == '\\') return "endereço contém caracteres inválidos";
    std::string rest;
    bool https = false;
    if (url.rfind("https://", 0) == 0) { rest = url.substr(8); https = true; }
    else if (url.rfind("http://", 0) == 0) rest = url.substr(7);
    else return "o endereço deve começar com https://";
    const auto slash = rest.find('/');
    const std::string host = rest.substr(0, slash);
    if (host.empty()) return "endereço sem servidor";
    if (!https && !isLoopbackHost(host)) return "http só é aceito para localhost; use https://";
    return {};
}

std::string transcriptionsUrl(const std::string& baseUrl) {
    std::string b = baseUrl;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b + "/audio/transcriptions";
}

bool CloudRecognizer::load(std::string& error) {
    if (!http_) { error = "cliente HTTP indisponível"; return false; }
    const std::string why = validateBaseUrl(cfg_.baseUrl);
    if (!why.empty()) { error = why; return false; }
    if (cfg_.apiKey.empty() && cfg_.baseUrl.rfind("https://", 0) == 0) { error = "informe a chave da API"; return false; }
    return true;
}

RecognizeResult CloudRecognizer::recognize(const RecognizeRequest& r) {
    RecognizeResult out;
    if (!http_ || !r.pcm || r.samples == 0) { out.error = "sem áudio"; return out; }
    const std::string boundary = "----hbstudio" + std::to_string(++counter_) + "x7d2f91c4a8";
    std::vector<std::pair<std::string, std::string>> fields = {{"model", cfg_.model}, {"response_format", "json"}};
    if (!r.language.empty() && r.language != "auto") fields.emplace_back("language", r.language);
    const std::string body =
        buildMultipart(boundary, fields, "file", "audio.wav", "audio/wav", encodeWav16(r.pcm, r.samples, kRecognizerSampleRate));
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "multipart/form-data; boundary=" + boundary}, {"Accept", "application/json"}};
    if (!cfg_.apiKey.empty()) headers.emplace_back("Authorization", "Bearer " + cfg_.apiKey);
    const HttpResponse resp = http_->post(transcriptionsUrl(cfg_.baseUrl), headers, body, cfg_.timeoutMs);
    if (!resp.error.empty()) { out.error = "rede: " + resp.error; return out; }
    if (resp.status == 401 || resp.status == 403) { out.error = "chave da API recusada (HTTP " + std::to_string(resp.status) + ")"; return out; }
    if (resp.status == 404) { out.error = "o servidor não oferece /audio/transcriptions (HTTP 404)"; return out; }
    if (resp.status < 200 || resp.status >= 300) { out.error = "servidor respondeu HTTP " + std::to_string(resp.status); return out; }
    if (!parseTranscriptionJson(resp.body, out.text)) { out.error = "resposta inesperada do servidor"; return out; }
    out.ok = true;
    return out;
}

}  // namespace hb::captions
