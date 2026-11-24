#include "captions/whisper_recognizer.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include "whisper.h"

namespace hb::captions {

struct WhisperRecognizer::Impl {
    whisper_context* ctx = nullptr;
    ~Impl() { if (ctx) whisper_free(ctx); }
};

WhisperRecognizer::WhisperRecognizer(std::string modelPathUtf8, int threads)
    : impl_(std::make_unique<Impl>()), modelPath_(std::move(modelPathUtf8)), threads_(threads) {
    if (threads_ <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        threads_ = static_cast<int>(std::clamp<unsigned>(hc / 2, 1, 4));
    }
}

WhisperRecognizer::~WhisperRecognizer() = default;

bool WhisperRecognizer::load(std::string& error) {
    if (impl_->ctx) return true;
    try {
        whisper_context_params cp = whisper_context_default_params();
        cp.use_gpu = false;   // CPU only: no GPU backend is compiled in
        impl_->ctx = whisper_init_from_file_with_params(modelPath_.c_str(), cp);
    } catch (...) {
        impl_->ctx = nullptr;
    }
    if (!impl_->ctx) {
        error = "não foi possível carregar o modelo de voz (arquivo ausente, corrompido ou memória insuficiente)";
        return false;
    }
    return true;
}

RecognizeResult WhisperRecognizer::recognize(const RecognizeRequest& r) {
    RecognizeResult out;
    if (!impl_->ctx) { out.error = "modelo não carregado"; return out; }
    if (!r.pcm || r.samples == 0) { out.error = "sem áudio"; return out; }
    try {
        // whisper rejects input shorter than 1 s: pad with silence.
        std::vector<float> pad;
        const float* data = r.pcm;
        std::size_t n = r.samples;
        constexpr std::size_t kMin = 16000 + 1600;
        if (n < kMin) {
            pad.assign(kMin, 0.0f);
            std::copy(r.pcm, r.pcm + n, pad.begin());
            data = pad.data();
            n = kMin;
        }
        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        p.n_threads = threads_;
        p.print_progress = false;
        p.print_realtime = false;
        p.print_special = false;
        p.print_timestamps = false;
        p.translate = false;
        p.no_context = true;          // utterances are independent
        p.no_timestamps = true;
        p.single_segment = r.partial; // interim decodes: one segment, cheaper
        p.suppress_blank = true;
        p.suppress_nst = true;        // no [MUSIC]-style tokens
        p.temperature_inc = 0.0f;     // no re-decode fallback: bounded latency
        p.language = r.language.empty() ? "pt" : r.language.c_str();   // "auto" = detect
        p.detect_language = (r.language == "auto");
        if (whisper_full(impl_->ctx, p, data, static_cast<int>(n)) != 0) { out.error = "falha na inferência"; return out; }
        const int segs = whisper_full_n_segments(impl_->ctx);
        for (int i = 0; i < segs; ++i) {
            const char* t = whisper_full_get_segment_text(impl_->ctx, i);
            if (!t) continue;
            if (!out.text.empty()) out.text.push_back(' ');
            out.text += t;
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = std::string("exceção: ") + e.what();
    } catch (...) {
        out.ok = false;
        out.error = "exceção desconhecida";
    }
    return out;
}

}  // namespace hb::captions
