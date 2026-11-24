#pragma once
// ChromaService: owns one ChromaFilter per input slot, the model store, the (lazily created)
// shared segmenter, and turns UI commands into settings changes. Portable: everything platform
// specific (HTTP, ONNX runtime, image decoding) is injected.
//
// Commands (UI -> host, already split on '|' by app_main):
//   chroma|<slot>|<key>|<value>   key: mode tol soft spill q repl color blur img   (see types.hpp setField)
//   chromamodel|download | cancel | remove
// Status (host -> UI): statusJson() -> {"t":"chroma", ...}
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chroma/chroma_filter.hpp"
#include "chroma/model_store.hpp"

namespace hb::chroma {

class ChromaService {
public:
    // Creates the runtime segmenter for a verified model file; null + err when impossible.
    using SegmenterFactory = std::function<std::shared_ptr<ISegmenter>(const std::string& modelPath, std::string& err)>;
    // Decodes an image file into an opaque BGRA frame; false + err on failure.
    using ImageLoader = std::function<bool(const std::string& path, video::Frame& out, std::string& err)>;
    // Called (from the command-handling thread) after a slot's settings changed: persist `serialized`.
    using ChangedFn = std::function<void(int slot, const std::string& serialized)>;

    struct Deps {
        std::string modelsDir;
        IHttpClient* http = nullptr;             // null: download unavailable
        SegmenterFactory segmenterFactory;       // empty: this build has no AI runtime (classic chroma only)
        ImageLoader imageLoader;                 // empty: image replacement unavailable
        ChangedFn onChanged;
        ModelInfo model = defaultModel();
    };

    ChromaService(int slots, Deps deps);
    ~ChromaService();

    std::shared_ptr<ChromaFilter> filter(int slot) const;
    int slotCount() const { return static_cast<int>(filters_.size()); }
    bool aiRuntimeAvailable() const { return static_cast<bool>(deps_.segmenterFactory); }

    // Restores persisted settings (no onChanged callback). Safe with garbage input.
    void load(int slot, const std::string& serialized);
    // Handles "chroma|slot|key|value". Returns false when the command is invalid; `message` is a
    // pt-BR toast/error text (may be set on success too, e.g. a warning).
    bool handleSetting(int slot, const std::string& key, const std::string& value, std::string& message);
    // Handles "chromamodel|action".
    bool handleModel(const std::string& action, std::string& message);
    // Resets a slot to defaults (Off). Used when its camera assignment is cleared.
    void reset(int slot);

    // Call at ~4 Hz from any non-realtime thread: starts the segmenter loader once the model is ready.
    void tick();
    std::string statusJson();
    void shutdown();

    ModelStore& model() { return store_; }

private:
    void ensureSegmenter();
    void loadImageFor(int slot, const std::string& path);

    Deps deps_;
    ModelStore store_;
    std::vector<std::shared_ptr<ChromaFilter>> filters_;

    mutable std::mutex m_;
    std::vector<std::string> imageErr_;
    enum class AiState { Idle, Loading, Ready, Error } aiState_ = AiState::Idle;
    std::string aiMsg_;
    std::shared_ptr<ISegmenter> segmenter_;
    std::thread loader_;
};

}  // namespace hb::chroma
