#include "streaming/windows/mf_encoder.hpp"

// TODO(mf-rtmp-encoder): real IMFTransform-driven H.264 encoding (see mf_encoder.hpp's header
// comment for why this can't reuse MfWriter/IMFSinkWriter). Placeholder keeps the Windows build
// green and reports failure honestly (never silently pretends to encode) until it lands.
namespace hb::win {

namespace {
class PlaceholderH264Encoder final : public IH264Encoder {
public:
    bool start(int, int, int, int, int, const std::string&) override {
        error_ = "Codificador H.264 para transmissão ainda não implementado nesta versão.";
        return false;
    }
    void stop() override {}
    bool isRunning() const override { return false; }
    bool encode(const std::uint8_t*, int, int, int, std::uint32_t, const std::function<void(const EncodedVideoUnit&)>&) override {
        return false;
    }
    bool spsPps(std::vector<std::uint8_t>&, std::vector<std::uint8_t>&) const override { return false; }
    std::string activeEncoderName() const override { return ""; }
    std::string lastError() const override { return error_; }

private:
    std::string error_;
};
}  // namespace

std::unique_ptr<IH264Encoder> makeH264Encoder() { return std::make_unique<PlaceholderH264Encoder>(); }

}  // namespace hb::win
