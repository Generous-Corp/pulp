#pragma once

// Where the design-import benchmark paints a frame, and the resource work it
// reads around each frame.
//
// The recording target sees only the paint-command stream. The Skia targets
// paint through SkiaCanvas, into a CPU raster surface or a headless Graphite
// surface on a Dawn device, so image decodes, GPU uploads, paragraph builds
// and rasterization are inside the measured frame and visible to the
// resource counters.

#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::design_import_bench {

class FrameTarget {
  public:
    virtual ~FrameTarget() = default;
    virtual const char* name() const = 0;
    // Whether this target performs decodes, uploads and text shaping at all;
    // a target that cannot is blind to the steady-state resource gate.
    virtual bool observes_resources() const = 0;
    // The canvas for one frame.
    virtual canvas::Canvas& begin() = 0;
    // Submits the frame and returns the paint commands it recorded, or 0 when
    // the target cannot count them.
    virtual std::uint64_t end() = 0;
};

// Records into a fresh RecordingCanvas per frame.
class RecordingTarget final : public FrameTarget {
  public:
    const char* name() const override {
        return "recording";
    }
    bool observes_resources() const override {
        return false;
    }
    canvas::Canvas& begin() override {
        canvas_.emplace();
        return *canvas_;
    }
    std::uint64_t end() override {
        return static_cast<std::uint64_t>(canvas_->command_count());
    }

  private:
    std::optional<canvas::RecordingCanvas> canvas_;
};

// `recording`, `skia-raster` or `skia-graphite`, sized to the fixture. A
// target this build or host cannot provide returns null with the reason; the
// benchmark never substitutes another target under the requested name.
std::unique_ptr<FrameTarget> make_frame_target(std::string_view name, std::uint32_t width,
                                               std::uint32_t height, std::string& error);

// Process-wide resource work at one instant (see pulp/canvas/resource_counters.hpp),
// plus the image cache's own hit and build counts.
struct ResourceSample {
    std::uint64_t image_decodes = 0;
    std::uint64_t gpu_uploads = 0;
    std::uint64_t paragraph_builds = 0;
    std::uint64_t scene_records = 0;
    std::uint64_t scene_replays = 0;
    std::uint64_t image_cache_hits = 0;
    std::uint64_t image_cache_builds = 0;
};
ResourceSample sample_resources();

} // namespace pulp::design_import_bench
