#include "streaming/rtmp_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include "streaming/flv.hpp"

namespace hb::streaming {

namespace {
constexpr std::uint8_t kChunkControl = 2, kChunkCommand = 3, kChunkVideo = 4, kChunkAudio = 5;
constexpr std::uint8_t kMsgSetChunkSize = 1, kMsgWindowAckSize = 5, kMsgSetPeerBandwidth = 6, kMsgAudio = 8,
                       kMsgVideo = 9, kMsgCommandAmf0 = 20;

void put24be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put32be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
void put32le(std::vector<std::uint8_t>& out, std::uint32_t v) {   // RTMP message stream id: little-endian (spec quirk)
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
std::uint32_t get24be(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2]; }
std::uint32_t get32le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

ParsedRtmpUrl parseRtmpUrl(const std::string& url) {
    ParsedRtmpUrl out;
    std::string rest = url;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) return out;   // no host, or no app segment at all
    std::string hostPort = rest.substr(0, slash);
    out.app = rest.substr(slash + 1);
    if (out.app.empty()) return out;
    const std::size_t colon = hostPort.find(':');
    if (colon == std::string::npos) {
        out.host = hostPort;
    } else {
        out.host = hostPort.substr(0, colon);
        const std::string portStr = hostPort.substr(colon + 1);
        if (portStr.empty() || portStr.find_first_not_of("0123456789") != std::string::npos) return out;
        const int p = std::atoi(portStr.c_str());
        if (p <= 0 || p > 65535) return out;
        out.port = static_cast<std::uint16_t>(p);
    }
    if (out.host.empty()) return out;
    out.ok = true;
    return out;
}

RtmpClient::RtmpClient(std::unique_ptr<hb::rtsp::ITcpStream> transport)
    : io_(transport ? std::move(transport) : hb::rtsp::makeSocketTcpStream()) {}

RtmpClient::~RtmpClient() { stop(); }

void RtmpClient::stop() {
    publishing_ = false;
    if (io_) io_->close();
    chunkStates_.clear();
}

bool RtmpClient::fail(const std::string& msg) {
    error_ = msg;
    publishing_ = false;
    return false;
}

bool RtmpClient::readExact(std::uint8_t* buf, std::size_t len, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::size_t got = 0;
    while (got < len) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return fail("tempo esgotado aguardando dados do servidor RTMP.");
        const int remainMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        std::size_t n = 0;
        const auto st = io_->recv(buf + got, len - got, n);
        if (st == hb::rtsp::IoStatus::Ok) {
            got += n;
        } else if (st == hb::rtsp::IoStatus::WouldBlock) {
            io_->waitReadable(std::clamp(remainMs, 1, 200));
        } else {
            return fail(st == hb::rtsp::IoStatus::Closed ? "o servidor RTMP fechou a conexão." : "falha de rede: " + io_->lastError());
        }
    }
    return true;
}

bool RtmpClient::writeExact(const std::uint8_t* buf, std::size_t len) {
    std::size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (off < len) {
        if (std::chrono::steady_clock::now() >= deadline) return fail("tempo esgotado enviando dados ao servidor RTMP.");
        std::size_t n = 0;
        const auto st = io_->send(buf + off, len - off, n);
        if (st == hb::rtsp::IoStatus::Ok) off += n;
        else if (st == hb::rtsp::IoStatus::WouldBlock) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        else return fail(st == hb::rtsp::IoStatus::Closed ? "o servidor RTMP fechou a conexão." : "falha de rede: " + io_->lastError());
    }
    return true;
}

bool RtmpClient::handshake(int timeoutMs) {
    std::vector<std::uint8_t> c1(1 + 1536);
    c1[0] = 0x03;   // C0: RTMP version 3 (plain, unencrypted) - what every real server still accepts
    std::uint8_t* c1Body = c1.data() + 1;
    std::memset(c1Body, 0, 8);   // time=0, zero=0 - the "simple handshake": no server needs these honoured
    std::mt19937 rng(std::random_device{}());
    for (int i = 8; i < 1536; ++i) c1Body[i] = static_cast<std::uint8_t>(rng());
    if (!writeExact(c1.data(), c1.size())) return false;

    std::uint8_t s0 = 0;
    if (!readExact(&s0, 1, timeoutMs)) return false;
    if (s0 != 0x03) return fail("o servidor respondeu com uma versão de handshake RTMP não suportada.");
    std::vector<std::uint8_t> s1(1536);
    if (!readExact(s1.data(), s1.size(), timeoutMs)) return false;
    std::vector<std::uint8_t> s2(1536);
    if (!readExact(s2.data(), s2.size(), timeoutMs)) return false;

    // C2 echoes S1 verbatim - the simple handshake never checks the digest the "complex" variant uses.
    return writeExact(s1.data(), s1.size());
}

bool RtmpClient::writeMessage(std::uint8_t chunkStreamId, std::uint8_t typeId, std::uint32_t streamId,
                              std::uint32_t timestampMs, const std::uint8_t* payload, std::size_t len) {
    const bool extTs = timestampMs >= 0xFFFFFF;
    std::vector<std::uint8_t> header;
    header.push_back(static_cast<std::uint8_t>(chunkStreamId & 0x3F));   // fmt=0 (top 2 bits 0), 1-byte basic header
    put24be(header, extTs ? 0xFFFFFF : timestampMs);
    put24be(header, static_cast<std::uint32_t>(len));
    header.push_back(typeId);
    put32le(header, streamId);
    if (extTs) put32be(header, timestampMs);
    if (!writeExact(header.data(), header.size())) return false;

    std::size_t off = 0;
    bool first = true;
    while (off < len || (len == 0 && first)) {
        if (!first) {
            std::vector<std::uint8_t> cont;
            cont.push_back(static_cast<std::uint8_t>(0xC0 | (chunkStreamId & 0x3F)));   // fmt=3: no header fields
            if (extTs) put32be(cont, timestampMs);   // fmt3 chunks of a message that used extended ts repeat it
            if (!writeExact(cont.data(), cont.size())) return false;
        }
        const std::size_t n = std::min<std::size_t>(outChunkSize_, len - off);
        if (n && !writeExact(payload + off, n)) return false;
        off += n;
        first = false;
        if (len == 0) break;
    }
    return true;
}

bool RtmpClient::readMessage(std::uint8_t& typeId, std::uint32_t& streamId, std::vector<std::uint8_t>& payload, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) return fail("tempo esgotado aguardando o servidor RTMP responder.");
        std::uint8_t b0;
        if (!readExact(&b0, 1, timeoutMs)) return false;
        const std::uint8_t fmt = static_cast<std::uint8_t>(b0 >> 6);
        std::uint32_t csid = b0 & 0x3F;
        if (csid == 0) {
            std::uint8_t ext;
            if (!readExact(&ext, 1, timeoutMs)) return false;
            csid = static_cast<std::uint32_t>(ext) + 64;
        } else if (csid == 1) {
            std::uint8_t ext[2];
            if (!readExact(ext, 2, timeoutMs)) return false;
            csid = static_cast<std::uint32_t>(ext[0]) + (static_cast<std::uint32_t>(ext[1]) << 8) + 64;
        }
        auto& st = chunkStates_[csid];   // default-constructed (all zero) the first time this csid is seen

        if (fmt <= 2) {
            std::uint8_t ts3[3];
            if (!readExact(ts3, 3, timeoutMs)) return false;
            std::uint32_t ts = get24be(ts3);
            if (fmt <= 1) {
                std::uint8_t rest[4];   // length(3) + typeId(1)
                if (!readExact(rest, 4, timeoutMs)) return false;
                st.length = get24be(rest);
                st.typeId = rest[3];
                if (fmt == 0) {
                    std::uint8_t sid[4];
                    if (!readExact(sid, 4, timeoutMs)) return false;
                    st.streamId = get32le(sid);
                }
            }
            st.extendedTimestamp = ts == 0xFFFFFF;
            if (st.extendedTimestamp) {
                std::uint8_t ext4[4];
                if (!readExact(ext4, 4, timeoutMs)) return false;
                ts = (static_cast<std::uint32_t>(ext4[0]) << 24) | (static_cast<std::uint32_t>(ext4[1]) << 16) |
                     (static_cast<std::uint32_t>(ext4[2]) << 8) | ext4[3];
            }
            st.timestamp = fmt == 2 ? st.timestamp + ts : ts;   // fmt1/fmt2 carry a DELTA, fmt0 an absolute value
            if (st.have == 0) { st.payload.assign(st.length, 0); }
        } else {   // fmt==3: continuation of the in-progress message, or a repeat of the previous header
            if (st.extendedTimestamp) {
                std::uint8_t ext4[4];   // real-world servers resend the extended timestamp on fmt3 chunks too
                if (!readExact(ext4, 4, timeoutMs)) return false;
            }
            if (st.have == 0) st.payload.assign(st.length, 0);   // a new message reusing the previous header verbatim
        }

        const std::size_t want = std::min<std::size_t>(inChunkSize_, st.length - st.have);
        if (want > 0 && !readExact(st.payload.data() + st.have, want, timeoutMs)) return false;
        st.have += want;
        if (st.have < st.length) continue;   // message spans more chunks

        st.have = 0;
        if (st.typeId == kMsgSetChunkSize && st.payload.size() >= 4) {
            inChunkSize_ = get24be(st.payload.data() + 1) | (static_cast<std::uint32_t>(st.payload[0]) << 24);
            inChunkSize_ &= 0x7FFFFFFF;
            if (inChunkSize_ == 0) inChunkSize_ = 128;
            continue;   // a protocol control message, not handed to the caller
        }
        if (st.typeId == kMsgWindowAckSize || st.typeId == kMsgSetPeerBandwidth) continue;   // acknowledged implicitly; we never throttle

        typeId = st.typeId;
        streamId = st.streamId;
        payload = std::move(st.payload);
        st.payload.clear();
        return true;
    }
}

bool RtmpClient::sendCommand(const std::string& name, double transactionId, const std::vector<std::uint8_t>& extraEncoded) {
    std::vector<std::uint8_t> body;
    amf0::encode(amf0::Value::string(name), body);
    amf0::encode(amf0::Value::number(transactionId), body);
    body.insert(body.end(), extraEncoded.begin(), extraEncoded.end());
    return writeMessage(kChunkCommand, kMsgCommandAmf0, 0, 0, body.data(), body.size());
}

bool RtmpClient::waitCommandReply(double transactionId, const char* matchName, int timeoutMs,
                                  std::vector<amf0::Value>& outValues) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return fail("tempo esgotado aguardando o servidor RTMP confirmar.");
        const int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        std::uint8_t typeId;
        std::uint32_t streamId;
        std::vector<std::uint8_t> payload;
        if (!readMessage(typeId, streamId, payload, std::max(remain, 1))) return false;
        if (typeId != kMsgCommandAmf0) continue;   // ignore anything that isn't an AMF0 command for now
        auto values = amf0::decodeAll(payload.data(), payload.size());
        if (values.empty() || values[0].type != amf0::Value::Type::String) continue;
        const std::string& cmd = values[0].str;
        if (matchName && cmd == matchName) { outValues = std::move(values); return true; }
        if ((cmd == "_result" || cmd == "_error") && values.size() > 1 && values[1].num == transactionId) {
            if (cmd == "_error") return fail("o servidor RTMP recusou o comando (_error).");
            outValues = std::move(values);
            return true;
        }
        // A command for a different transaction/stream (e.g. an earlier onStatus we don't need
        // right now): keep waiting, it's not an error.
    }
}

bool RtmpClient::start(const RtmpTarget& target, int timeoutMs) {
    error_.clear();
    publishing_ = false;
    chunkStates_.clear();

    auto cst = io_->connect(target.host, target.port);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (cst == hb::rtsp::IoStatus::WouldBlock) {
        if (std::chrono::steady_clock::now() >= deadline) return fail("tempo esgotado conectando ao servidor RTMP.");
        io_->waitReadable(50);
        cst = io_->pollConnect();
    }
    if (cst != hb::rtsp::IoStatus::Ok) return fail("não foi possível conectar: " + io_->lastError());

    if (!handshake(timeoutMs)) return false;

    // "Set Chunk Size" to something friendlier to 1080p NALUs than the 128-byte default, before any
    // media-sized message is sent.
    {
        std::vector<std::uint8_t> body;
        put32be(body, outChunkSize_);
        if (!writeMessage(kChunkControl, kMsgSetChunkSize, 0, 0, body.data(), body.size())) return false;
    }

    const std::string tcUrl = target.tcUrl.empty() ? ("rtmp://" + target.host + "/" + target.app) : target.tcUrl;
    std::vector<std::uint8_t> connectArgs;
    amf0::encode(amf0::Value::object({{"app", amf0::Value::string(target.app)},
                                      {"type", amf0::Value::string("nonprivate")},
                                      {"flashVer", amf0::Value::string("HBStudio/1.0")},
                                      {"tcUrl", amf0::Value::string(tcUrl)},
                                      {"fpad", amf0::Value::boolean_(false)}}),
               connectArgs);
    if (!sendCommand("connect", 1, connectArgs)) return false;
    std::vector<amf0::Value> reply;
    if (!waitCommandReply(1, nullptr, timeoutMs, reply)) return false;

    // createStream's single argument is a bare `null`, not an object.
    {
        std::vector<std::uint8_t> nullArg;
        amf0::encode(amf0::Value::null(), nullArg);
        if (!sendCommand("createStream", 2, nullArg)) return false;
    }
    std::vector<amf0::Value> created;
    if (!waitCommandReply(2, nullptr, timeoutMs, created)) return false;
    if (created.size() < 4 || created[3].type != amf0::Value::Type::Number)
        return fail("resposta inesperada do servidor a createStream.");
    streamId_ = static_cast<std::uint32_t>(created[3].num);

    std::vector<std::uint8_t> publishArgs;
    amf0::encode(amf0::Value::null(), publishArgs);
    amf0::encode(amf0::Value::string(target.streamKey), publishArgs);
    amf0::encode(amf0::Value::string("live"), publishArgs);
    {
        std::vector<std::uint8_t> body;
        amf0::encode(amf0::Value::string("publish"), body);
        amf0::encode(amf0::Value::number(0), body);   // publish's transaction id is conventionally 0
        body.insert(body.end(), publishArgs.begin(), publishArgs.end());
        if (!writeMessage(kChunkCommand, kMsgCommandAmf0, streamId_, 0, body.data(), body.size())) return false;
    }
    std::vector<amf0::Value> status;
    if (!waitCommandReply(0, "onStatus", timeoutMs, status)) return false;
    if (status.size() >= 4) {
        // onStatus signals failure via level:"error" (or "warning"), not by any particular prefix on
        // the "code" string (publish failure codes are e.g. "NetStream.Publish.BadName", which does
        // NOT start with the word "error") - the level field is the one spec-guaranteed signal.
        const auto* level = status[3].field("level");
        const auto* code = status[3].field("code");
        if (level && level->type == amf0::Value::Type::String && level->str != "status") {
            const std::string c = code && code->type == amf0::Value::Type::String ? code->str : "desconhecido";
            return fail("o servidor recusou a publicação (" + c + ").");
        }
    }

    publishing_ = true;
    return true;
}

bool RtmpClient::sendVideo(std::uint32_t timestampMs, const std::vector<std::uint8_t>& tagBody) {
    if (!publishing_) return false;
    if (!writeMessage(kChunkVideo, kMsgVideo, streamId_, timestampMs, tagBody.data(), tagBody.size())) {
        publishing_ = false;
        return false;
    }
    return true;
}

bool RtmpClient::sendAudio(std::uint32_t timestampMs, const std::vector<std::uint8_t>& tagBody) {
    if (!publishing_) return false;
    if (!writeMessage(kChunkAudio, kMsgAudio, streamId_, timestampMs, tagBody.data(), tagBody.size())) {
        publishing_ = false;
        return false;
    }
    return true;
}

}  // namespace hb::streaming
