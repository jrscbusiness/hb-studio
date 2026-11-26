#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rtsp/tcp_stream.hpp"
#include "streaming/amf0.hpp"
#include "streaming/tls_engine.hpp"
#include "streaming/transport.hpp"

// A from-scratch RTMP/RTMPS publish client: handshake, chunk stream (de)multiplexing, AMF0
// connect/createStream/publish, then raw video (type 9) / audio (type 8) message sending. Built on
// hb::rtsp::ITcpStream - the existing, already cross-platform (POSIX/Winsock) non-blocking TCP
// abstraction RTSP ingest uses - rather than introducing a second socket layer. RTMPS (rtmps://,
// default port 443) is RTMP over TLS: start() wraps the stream in a TlsStream (tls_stream.hpp) whose
// engine is Schannel on Windows (or an injected fake in tests). Certificate verification is always on.
//
// Deliberately simple/synchronous: start()/sendVideo()/sendAudio() block the calling thread (bounded
// by timeoutMs / the OS send buffer). This is correct and safe here because each destination gets
// its own dedicated sender thread (see StreamSender) - never the audio/video render thread - so one
// slow/stuck destination blocks only its own thread, matching the "a failure in one destination must
// not affect the others" requirement without needing a second, harder-to-get-right async state
// machine.
namespace hb::streaming {

struct ParsedRtmpUrl {
    std::string host;
    std::uint16_t port = 1935;   // 443 for rtmps:// when no port is written
    std::string app;   // everything after host[:port]/ - may itself contain slashes
    bool tls = false;            // scheme was rtmps://
    bool portExplicit = false;   // the URL spelled the port out
    bool ok = false;
};
// Parses "rtmp://host[:port]/app[/more]" and "rtmps://host[:port]/app[/more]" (no scheme = plain
// rtmp). rtmps sets tls=true and defaults the port to 443. ok=false for anything missing a host or a
// non-empty app segment, or with a scheme other than rtmp/rtmps.
ParsedRtmpUrl parseRtmpUrl(const std::string& url);

struct RtmpTarget {
    std::string host;
    std::uint16_t port = 1935;
    std::string app;         // the path segment(s) before the stream key, e.g. "live2" for YouTube
    std::string streamKey;
    std::string tcUrl;       // "rtmp[s]://host[:port]/app", what the connect() command announces (derived if empty)
    bool tls = false;        // RTMPS: wrap the TCP stream in TLS
};

class RtmpClient final : public IStreamTransport {
public:
    using TlsEngineFactory = std::function<std::unique_ptr<ITlsEngine>()>;
    // `transport`: nullptr uses a real socket (hb::rtsp::makeSocketTcpStream()); tests inject a fake
    // or a loopback hb::rtsp::ITcpStream instead.
    explicit RtmpClient(std::unique_ptr<hb::rtsp::ITcpStream> transport = nullptr);
    ~RtmpClient() override;
    RtmpClient(const RtmpClient&) = delete;
    RtmpClient& operator=(const RtmpClient&) = delete;

    // Overrides where the TLS engine comes from (tests inject a fake); default = makeSystemTlsEngine().
    void setTlsEngineFactory(TlsEngineFactory f) { tlsFactory_ = std::move(f); }

    bool start(const RtmpTarget& target, int timeoutMs = 8000);
    // IStreamTransport: parses `url` (rtmp:// or rtmps://) and calls start().
    bool open(const std::string& url, const std::string& streamKey, int timeoutMs) override;
    void stop() override;
    bool isPublishing() const override { return publishing_; }
    std::string lastError() const override { return error_; }
    bool permanentFailure() const override { return permanent_; }

    // `tagBody` is exactly what streaming::flv::build*() produces. Returns false (and sets
    // isPublishing() to false) on any write failure; the caller should stop() and may retry with a
    // fresh RtmpClient.
    bool sendVideo(std::uint32_t timestampMs, const std::vector<std::uint8_t>& tagBody) override;
    bool sendAudio(std::uint32_t timestampMs, const std::vector<std::uint8_t>& tagBody) override;

private:
    // Per chunk-stream-id state needed to de-chunk incoming messages (type 1/2/3 chunks omit fields
    // that repeat the previous chunk's header on the same chunk stream id) and to reassemble a
    // message's payload across however many chunks it was split into.
    struct ChunkStreamState {
        std::uint32_t timestamp = 0;
        std::uint32_t length = 0;
        std::uint8_t typeId = 0;
        std::uint32_t streamId = 0;
        bool extendedTimestamp = false;
        std::vector<std::uint8_t> payload;
        std::size_t have = 0;
    };

    bool fail(const std::string& msg);
    bool failPermanent(const std::string& msg);
    bool handshake(int timeoutMs);
    bool readExact(std::uint8_t* buf, std::size_t len, int timeoutMs);
    bool writeExact(const std::uint8_t* buf, std::size_t len);
    bool writeMessage(std::uint8_t chunkStreamId, std::uint8_t typeId, std::uint32_t streamId,
                      std::uint32_t timestampMs, const std::uint8_t* payload, std::size_t len);
    // Reads and reassembles the next complete message (across as many chunks as needed), honouring
    // any "Set Chunk Size" the peer sends and silently consuming window-ack-size/peer-bandwidth
    // control messages (type 5/6) - the caller never sees those. False on timeout/error.
    bool readMessage(std::uint8_t& typeId, std::uint32_t& streamId, std::vector<std::uint8_t>& payload, int timeoutMs);
    // Sends an AMF0 command (name + transactionId + already-encoded extra arguments) on chunk
    // stream id 3 / message stream id 0.
    bool sendCommand(const std::string& name, double transactionId, const std::vector<std::uint8_t>& extraEncoded);
    // Reads messages until one decodes to a command whose first value is `_result`/`_error` with a
    // matching transactionId, or (when matchName is set, used for onStatus) any command named
    // matchName - whichever comes first. Returns the full decoded value list (name, txnId/0,
    // ...args). False on timeout, transport error, or an explicit `_error` reply.
    bool waitCommandReply(double transactionId, const char* matchName, int timeoutMs,
                          std::vector<amf0::Value>& outValues);

    std::unique_ptr<hb::rtsp::ITcpStream> io_;
    std::string error_;
    bool publishing_ = false;
    bool permanent_ = false;
    bool tlsWrapped_ = false;
    TlsEngineFactory tlsFactory_;
    std::uint32_t outChunkSize_ = 4096;   // what WE send, announced to the peer right after connect()
    std::uint32_t inChunkSize_ = 128;     // what the PEER told us to expect; 128 until it says otherwise
    std::uint32_t streamId_ = 0;          // from createStream's _result; used for publish/video/audio
    std::map<std::uint32_t, ChunkStreamState> chunkStates_;
};

}  // namespace hb::streaming
