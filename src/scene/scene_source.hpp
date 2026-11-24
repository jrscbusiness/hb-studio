#pragma once
// SceneSource: a video::Source that renders a Scene, so a scene sits on the switcher bus like a
// camera (CUT / DISSOLVE / T-bar then work with it with no special case).
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scene/compositor.hpp"
#include "scene/scene.hpp"
#include "video/source.hpp"

namespace hb::scene {

class SceneSource final : public video::Source {
public:
    // `inputs[i]` renders input slot i (the 16 camera slots). `aspectOf(i)` may report a slot's native
    // width/height (<= 0 or an empty function means 16:9).
    SceneSource(std::string name, std::vector<std::shared_ptr<const video::Source>> inputs,
                std::function<double(int slot)> aspectOf = nullptr, ImageLoader loader = nullptr);

    const std::string& name() const override { return name_; }
    bool isTestSource() const override { return !hasScene(); }
    // Never blocks on I/O. An empty slot renders a calm dark panel.
    void render(video::Frame& frame, std::uint64_t index) const override;

    void setScene(std::shared_ptr<const Scene> scene);  // null = empty slot
    std::shared_ptr<const Scene> scene() const;
    bool hasScene() const { return static_cast<bool>(scene()); }

    // Pre-compiles for an output size so the first render at that size (possibly on the Program
    // clock thread) does not pay for masks/shadows/background. Safe from any thread.
    void prepare(int width, int height);

    std::uint64_t compileCount() const;  // for tests

private:
    struct CacheEntry {
        std::shared_ptr<const CompiledScene> cs;
        std::uint64_t used = 0;
    };
    std::vector<double> currentAspects(const Scene& s) const;
    std::shared_ptr<const CompiledScene> compiled(const std::shared_ptr<const Scene>& sc, int w, int h,
                                                  const std::vector<double>& aspects) const;

    std::string name_;
    std::vector<std::shared_ptr<const video::Source>> inputs_;
    std::function<double(int)> aspectOf_;
    ImageLoader loader_;

    mutable std::mutex mutex_;
    std::shared_ptr<const Scene> scene_;
    mutable std::vector<CacheEntry> cache_;   // entries belong to scene_
    mutable std::uint64_t tick_ = 0, compiles_ = 0;
};

}  // namespace hb::scene
