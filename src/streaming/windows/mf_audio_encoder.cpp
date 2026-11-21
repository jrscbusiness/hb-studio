#include "streaming/windows/mf_encoder.hpp"

// TODO(mf-rtmp-encoder): real IMFTransform-driven AAC encoding. See mf_video_encoder.cpp's note -
// same reasoning, same placeholder approach (fails honestly rather than pretending to encode).
namespace hb::win {

namespace {
class PlaceholderAacEncoder final : public IAacEncoder {
public:
    bool start(int, int, int) override {
        error_ = "Codificador AAC para transmissão ainda não implementado nesta versão.";
        return false;
    }
    void stop() override {}
    bool isRunning() const override { return false; }
    bool encode(const float*, std::size_t, std::uint32_t, const std::function<void(const EncodedAudioUnit&)>&) override {
        return false;
    }
    bool audioSpecificConfig(std::vector<std::uint8_t>&) const override { return false; }
    std::string lastError() const override { return error_; }

private:
    std::string error_;
};
}  // namespace

std::unique_ptr<IAacEncoder> makeAacEncoder() { return std::make_unique<PlaceholderAacEncoder>(); }

}  // namespace hb::win
