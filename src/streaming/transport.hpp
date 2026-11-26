#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The small seam between "what a destination does" (publish FLV-style video/audio message bodies to
// some server) and "how bytes get there" (RTMP over TCP, RTMPS = RTMP over TLS, and later SRT).
// StreamSender and ReconnectingPublisher only know this interface, so adding a protocol is: implement
// IStreamTransport, teach makeStreamTransport() to build it. RtmpClient is the only implementation
// today; SRT is a documented stub (docs/SRT.md) - makeStreamTransport("srt") returns nullptr.
namespace hb::streaming {

class IStreamTransport {
public:
    virtual ~IStreamTransport() = default;
    // Connects and starts publishing. `url` is the server address WITHOUT the stream key
    // (e.g. "rtmps://a.rtmps.youtube.com:443/live2"). Blocks up to roughly timeoutMs per network step.
    virtual bool open(const std::string& url, const std::string& streamKey, int timeoutMs) = 0;
    virtual void stop() = 0;
    virtual bool isPublishing() const = 0;
    virtual std::string lastError() const = 0;   // pt-BR, for the operator
    // True when retrying cannot help (server refused the key/app, TLS certificate rejected,
    // malformed URL, protocol not available). The reconnect loop gives up instead of hammering.
    virtual bool permanentFailure() const = 0;
    // `body` is exactly what streaming::flv::build*() produces (FLV tag body without the tag header).
    virtual bool sendVideo(std::uint32_t timestampMs, const std::vector<std::uint8_t>& body) = 0;
    virtual bool sendAudio(std::uint32_t timestampMs, const std::vector<std::uint8_t>& body) = 0;
};

// protocol: "rtmp" | "rtmps" | "srt". Returns nullptr for a protocol with no implementation
// (today: "srt"); `whyNot` then says so in pt-BR. The URL scheme, not `protocol`, decides TLS for
// RTMP(S) - see RtmpClient::open - so a destination whose URL says rtmps:// is always encrypted.
std::unique_ptr<IStreamTransport> makeStreamTransport(const std::string& protocol, std::string* whyNot = nullptr);

}  // namespace hb::streaming
