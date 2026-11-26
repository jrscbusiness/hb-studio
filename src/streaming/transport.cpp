#include "streaming/transport.hpp"

#include "streaming/rtmp_client.hpp"

namespace hb::streaming {

std::unique_ptr<IStreamTransport> makeStreamTransport(const std::string& protocol, std::string* whyNot) {
    if (protocol == "rtmp" || protocol == "rtmps") return std::make_unique<RtmpClient>();
    if (whyNot) {
        *whyNot = protocol == "srt"
                      ? "SRT ainda não é suportado nesta versão (veja docs/SRT.md). Use RTMP ou RTMPS."
                      : "Protocolo de transmissão desconhecido: \"" + protocol + "\".";
    }
    return nullptr;
}

}  // namespace hb::streaming
