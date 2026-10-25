#include "tools/import-design/design_import_benchmark_targets.hpp"

#include <pulp/canvas/resource_counters.hpp>

#include <stdexcept>
#include <utility>

#if defined(PULP_HAS_SKIA)
#include <pulp/canvas/image_file_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#endif

namespace pulp::design_import_bench {

#if defined(PULP_HAS_SKIA)
namespace {

// A CPU raster surface reused across frames, with a fresh SkiaCanvas per
// frame as a live renderer creates one.
class SkiaRasterTarget final : public FrameTarget {
  public:
    explicit SkiaRasterTarget(sk_sp<SkSurface> surface) : surface_(std::move(surface)) {}
    const char* name() const override {
        return "skia-raster";
    }
    bool observes_resources() const override {
        return true;
    }
    canvas::Canvas& begin() override {
        auto* sk_canvas = surface_->getCanvas();
        sk_canvas->clear(SK_ColorBLACK);
        canvas_.emplace(sk_canvas);
        return *canvas_;
    }
    std::uint64_t end() override {
        canvas_.reset();
        return 0;
    }

  private:
    sk_sp<SkSurface> surface_;
    std::optional<canvas::SkiaCanvas> canvas_;
};

// Skia Graphite on a Dawn device with no presentation surface. end() submits
// the recording; it does not wait for the GPU, as a presenting host does not.
class SkiaGraphiteTarget final : public FrameTarget {
  public:
    SkiaGraphiteTarget(std::unique_ptr<render::GpuSurface> gpu,
                       std::unique_ptr<render::SkiaSurface> skia, std::uint32_t width,
                       std::uint32_t height)
        : gpu_(std::move(gpu)), skia_(std::move(skia)), width_(width), height_(height) {}
    ~SkiaGraphiteTarget() override {
        // Images uploaded through this device must not outlive it.
        canvas::ImageFileCache::instance().clear();
        skia_.reset();
        gpu_.reset();
    }
    const char* name() const override {
        return "skia-graphite";
    }
    bool observes_resources() const override {
        return true;
    }
    canvas::Canvas& begin() override {
        if (!gpu_->begin_frame())
            throw std::runtime_error("GpuSurface::begin_frame failed");
        auto* canvas = skia_->begin_frame();
        if (!canvas) {
            gpu_->end_frame();
            throw std::runtime_error("SkiaSurface::begin_frame returned no canvas");
        }
        canvas->set_fill_color(canvas::Color::rgba8(0, 0, 0, 255));
        canvas->fill_rect(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        return *canvas;
    }
    std::uint64_t end() override {
        const auto outcome = skia_->end_frame();
        gpu_->end_frame();
        if (!render::frame_reached_output(outcome))
            throw std::runtime_error("Graphite frame did not reach its output");
        return 0;
    }

  private:
    std::unique_ptr<render::GpuSurface> gpu_;
    std::unique_ptr<render::SkiaSurface> skia_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace
#endif

std::unique_ptr<FrameTarget> make_frame_target(std::string_view name, std::uint32_t width,
                                               std::uint32_t height, std::string& error) {
    if (name == "recording")
        return std::make_unique<RecordingTarget>();
#if defined(PULP_HAS_SKIA)
    if (name == "skia-raster") {
        auto surface = SkSurfaces::Raster(
            SkImageInfo::MakeN32Premul(static_cast<int>(width), static_cast<int>(height)));
        if (!surface) {
            error = "could not allocate a raster surface";
            return nullptr;
        }
        return std::make_unique<SkiaRasterTarget>(std::move(surface));
    }
    if (name == "skia-graphite") {
        auto gpu = render::GpuSurface::create_dawn();
        render::GpuSurface::Config config{};
        config.width = width;
        config.height = height;
        config.vsync = false;
        if (!gpu || !gpu->initialize(config)) {
            error = "no Dawn device (GpuSurface::initialize failed)";
            return nullptr;
        }
        const auto adapter = gpu->adapter_info();
        if (!adapter.available || adapter.null_backend) {
            error = "no native GPU adapter (Dawn Null backend)";
            return nullptr;
        }
        auto skia = render::SkiaSurface::create(*gpu, {width, height, 1.0f});
        if (!skia || !skia->is_available()) {
            error = "SkiaSurface unavailable on this device";
            return nullptr;
        }
        return std::make_unique<SkiaGraphiteTarget>(std::move(gpu), std::move(skia), width, height);
    }
#endif
    if (name == "skia-raster" || name == "skia-graphite")
        error = "this build has no Skia backend";
    else
        error = "unknown canvas '" + std::string(name) + "'";
    return nullptr;
}

ResourceSample sample_resources() {
    const auto counters = canvas::resource_counters();
    ResourceSample sample;
    sample.image_decodes = counters.image_decodes;
    sample.gpu_uploads = counters.gpu_uploads;
    sample.paragraph_builds = counters.paragraph_builds;
    sample.scene_records = counters.scene_records;
    sample.scene_replays = counters.scene_replays;
#if defined(PULP_HAS_SKIA)
    const auto cache = canvas::ImageFileCache::instance().stats();
    sample.image_cache_hits = cache.hits;
    sample.image_cache_builds = cache.builds;
#endif
    return sample;
}

} // namespace pulp::design_import_bench
