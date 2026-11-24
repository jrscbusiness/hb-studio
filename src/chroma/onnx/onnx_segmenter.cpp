#include "chroma/onnx/onnx_segmenter.hpp"

#include <windows.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "chroma/mask_ops.hpp"

namespace hb::chroma {

namespace {

class OnnxSegmenter final : public ISegmenter {
public:
    explicit OnnxSegmenter(const std::wstring& path) : env_(ORT_LOGGING_LEVEL_WARNING, "hbstudio") {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(2);   // modest PCs: never let one camera's AI take every core
        so.SetInterOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = Ort::Session(env_, path.c_str(), so);

        Ort::AllocatorWithDefaultOptions alloc;
        if (session_.GetInputCount() < 1 || session_.GetOutputCount() < 1) throw std::runtime_error("modelo sem entrada/saída");
        inName_ = session_.GetInputNameAllocated(0, alloc).get();
        outName_ = session_.GetOutputNameAllocated(0, alloc).get();
        const auto shape = session_.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 4) {
            if (shape[1] == 3) {   // NCHW (the pinned model)
                if (shape[2] > 0) h_ = static_cast<int>(shape[2]);
                if (shape[3] > 0) w_ = static_cast<int>(shape[3]);
            } else if (shape[3] == 3) {   // NHWC
                nhwc_ = true;
                if (shape[1] > 0) h_ = static_cast<int>(shape[1]);
                if (shape[2] > 0) w_ = static_cast<int>(shape[2]);
            } else {
                throw std::runtime_error("formato de entrada do modelo não suportado");
            }
        } else {
            throw std::runtime_error("formato de entrada do modelo não suportado");
        }
        if (w_ < 16 || h_ < 16 || w_ > 2048 || h_ > 2048) throw std::runtime_error("tamanho de entrada do modelo inesperado");
    }

    int inputWidth() const override { return w_; }
    int inputHeight() const override { return h_; }

    bool segment(const video::Frame& in, Mask& out, std::string& err) override {
        if (in.width != w_ || in.height != h_ || in.data.size() < static_cast<std::size_t>(in.stride) * in.height) {
            err = "quadro de entrada com tamanho errado";
            return false;
        }
        try {
            const std::size_t plane = static_cast<std::size_t>(w_) * h_;
            std::vector<float> buf(plane * 3);
            constexpr float k = 1.f / 255.f;   // the model card: rescale 1/255, no mean/std normalisation, RGB
            for (int y = 0; y < h_; ++y) {
                const std::uint8_t* p = in.data.data() + static_cast<std::size_t>(y) * in.stride;
                for (int x = 0; x < w_; ++x, p += 4) {
                    const std::size_t i = static_cast<std::size_t>(y) * w_ + x;
                    if (nhwc_) {
                        buf[i * 3 + 0] = p[2] * k;
                        buf[i * 3 + 1] = p[1] * k;
                        buf[i * 3 + 2] = p[0] * k;
                    } else {
                        buf[i] = p[2] * k;
                        buf[plane + i] = p[1] * k;
                        buf[2 * plane + i] = p[0] * k;
                    }
                }
            }
            const std::array<int64_t, 4> shape = nhwc_ ? std::array<int64_t, 4>{1, h_, w_, 3} : std::array<int64_t, 4>{1, 3, h_, w_};
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value input = Ort::Value::CreateTensor<float>(mem, buf.data(), buf.size(), shape.data(), shape.size());
            const char* inNames[] = {inName_.c_str()};
            const char* outNames[] = {outName_.c_str()};
            auto outputs = session_.Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
            if (outputs.empty() || !outputs[0].IsTensor()) {
                err = "saída do modelo inválida";
                return false;
            }
            const auto info = outputs[0].GetTensorTypeAndShapeInfo();
            const auto dims = info.GetShape();
            if (dims.size() < 2) {
                err = "saída do modelo inválida";
                return false;
            }
            const int oh = static_cast<int>(dims[dims.size() - 2]), ow = static_cast<int>(dims[dims.size() - 1]);
            if (oh <= 0 || ow <= 0 || static_cast<std::size_t>(oh) * ow > 4u * 1024 * 1024 ||
                info.GetElementCount() < static_cast<std::size_t>(oh) * ow) {
                err = "tamanho de saída do modelo inesperado";
                return false;
            }
            const float* d = outputs[0].GetTensorData<float>();
            const std::size_t n = static_cast<std::size_t>(oh) * ow;
            float lo = d[0], hi = d[0];
            for (std::size_t i = 1; i < n; ++i) {
                lo = std::min(lo, d[i]);
                hi = std::max(hi, d[i]);
            }
            const bool logits = lo < -0.01f || hi > 1.01f;   // the pinned model already outputs 0..1; be safe if not
            out.resize(ow, oh);
            for (std::size_t i = 0; i < n; ++i) {
                const float v = logits ? 1.f / (1.f + std::exp(-d[i])) : d[i];
                out.data[i] = probToByte(v);
            }
            return true;
        } catch (const Ort::Exception& e) {
            err = std::string("ONNX Runtime: ") + e.what();
            return false;
        }
    }

private:
    Ort::Env env_;
    Ort::Session session_{nullptr};
    std::string inName_, outName_;
    int w_ = 256, h_ = 144;
    bool nhwc_ = false;
};

}  // namespace

std::shared_ptr<ISegmenter> createOnnxSegmenter(const std::string& modelPath, std::string& err) {
    // onnxruntime.dll is delay-loaded so the app still starts without it. Probe it first, restricted
    // to the application directory (no DLL search-path hijacking), because a failed delay-load is an
    // SEH exception, not a C++ one.
    HMODULE dll = LoadLibraryExW(L"onnxruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!dll) {
        err = "onnxruntime.dll não encontrada ao lado do HBStudio.exe";
        return nullptr;
    }
    try {
        const int n = MultiByteToWideChar(CP_UTF8, 0, modelPath.data(), static_cast<int>(modelPath.size()), nullptr, 0);
        if (n <= 0) {
            err = "caminho do modelo inválido";
            return nullptr;
        }
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, modelPath.data(), static_cast<int>(modelPath.size()), &w[0], n);
        return std::make_shared<OnnxSegmenter>(w);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "erro desconhecido";
    }
    return nullptr;
}

}  // namespace hb::chroma
