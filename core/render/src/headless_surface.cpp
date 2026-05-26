// headless_surface.cpp — CI-friendly wrapper around the existing
// offscreen Dawn/Skia path. Spec: planning/2026-05-24-macos-plugin-
// authoring-plan.md §6.7 ("upgrade Headless GpuSurface from Partial →
// Full — capability already exists; this is wrapping").
//
// We deliberately do NOT introduce a new render path here. All of the
// real GPU work continues to live in `GpuSurface::create_dawn()` +
// `SkiaSurface::create()`. This translation unit:
//
//   1. Stitches the two together with `native_surface_handle=nullptr`
//      (Dawn's documented offscreen mode).
//   2. Brackets the user paint callback with begin_frame/end_frame
//      and a deterministic background clear.
//   3. Pulls pixels off the GPU via `SkiaSurface::read_current_rgba`
//      and (optionally) PNG-encodes them with `SkPngEncoder`.
//   4. When `PULP_HAS_SKIA` is OFF, returns a sentinel "unavailable"
//      surface — callers soft-skip via `is_ready()`/`last_error()`.
//
// PNG encoding goes through Skia's `SkPngEncoder` rather than the
// macOS-only `mac_capture::encode_rgba_to_png` helper so the same
// wrapper produces deterministic bytes on Linux runners too.

#include <pulp/render/headless_surface.hpp>

#include <pulp/canvas/canvas.hpp>

#if defined(PULP_HAS_SKIA)
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"
#endif

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::render {

namespace {

#if defined(PULP_HAS_SKIA)

class HeadlessSurfaceImpl final : public HeadlessSurface {
public:
    HeadlessSurfaceImpl(std::unique_ptr<GpuSurface> gpu,
                        std::unique_ptr<SkiaSurface> skia,
                        Config config)
        : gpu_(std::move(gpu)),
          skia_(std::move(skia)),
          config_(config) {}

    bool is_ready() const override {
        return gpu_ && skia_ && skia_->is_available();
    }

    Rgba render_rgba(const PaintFn& paint) override {
        last_error_.clear();
        Rgba out;
        if (!is_ready()) {
            last_error_ = "HeadlessSurface: not ready (GpuSurface or SkiaSurface unavailable)";
            return out;
        }
        if (!gpu_->begin_frame()) {
            last_error_ = "HeadlessSurface: GpuSurface::begin_frame() failed";
            return out;
        }
        auto* canvas = skia_->begin_frame();
        if (canvas == nullptr) {
            last_error_ = "HeadlessSurface: SkiaSurface::begin_frame() returned null canvas";
            // Still end the gpu frame so the device queue isn't left mid-frame.
            gpu_->end_frame();
            return out;
        }

        // Deterministic background fill. Without this the GPU texture
        // we read back may contain uninitialized memory and the PNG
        // bytes won't be reproducible across reruns (plan §6.7 acceptance).
        canvas->set_fill_color(canvas::Color::rgba8(
            config_.clear_r, config_.clear_g, config_.clear_b, config_.clear_a));
        canvas->fill_rect(0.0f, 0.0f,
                          static_cast<float>(config_.width),
                          static_cast<float>(config_.height));

        if (paint) {
            paint(*canvas);
        }

        // Submit the recorded GPU work BEFORE reading pixels back.
        // `SkiaSurface::end_frame()` is what calls `recorder_->snap()` +
        // `context_->insertRecording()` + `context_->submit()`. If we
        // tried to `read_current_rgba` before that, the readback would
        // sample whatever was in the offscreen render target on the
        // previous frame (or uninitialized memory on the very first
        // frame) — which is exactly the determinism bug this wrapper
        // is supposed to make impossible.
        //
        // `end_frame()` only resets the per-frame `frame_surface_` (the
        // wrapped swapchain texture); the offscreen `offscreen_surface_`
        // we render into here survives, so `read_current_rgba` still
        // has a valid source after the submit.
        skia_->end_frame();

        std::vector<uint8_t> pixels;
        uint32_t pw = 0, ph = 0;
        const bool read = skia_->read_current_rgba(pixels, pw, ph);

        gpu_->end_frame();

        if (!read) {
            last_error_ = "HeadlessSurface: SkiaSurface::read_current_rgba() failed (GPU readback unsupported on this adapter?)";
            return out;
        }
        out.pixels = std::move(pixels);
        out.width = pw;
        out.height = ph;
        return out;
    }

    std::vector<uint8_t> render_png(const PaintFn& paint) override {
        auto rgba = render_rgba(paint);
        if (rgba.empty()) return {};
        std::string enc_err;
        auto png = HeadlessSurface::encode_png(rgba, &enc_err);
        if (png.empty() && !enc_err.empty() && last_error_.empty()) {
            last_error_ = enc_err;
        }
        return png;
    }

    std::string last_error() const override { return last_error_; }

private:
    std::unique_ptr<GpuSurface> gpu_;
    std::unique_ptr<SkiaSurface> skia_;
    Config config_;
    mutable std::string last_error_;
};

#else  // !PULP_HAS_SKIA

// Sentinel: callers can always construct a HeadlessSurface for type
// reasons but `is_ready()` returns false and `last_error()` carries
// a clear "skia not linked" reason so tests soft-skip rather than
// fail compilation.
class HeadlessSurfaceImpl final : public HeadlessSurface {
public:
    HeadlessSurfaceImpl() = default;
    bool is_ready() const override { return false; }
    Rgba render_rgba(const PaintFn&) override {
        last_error_ = "HeadlessSurface: built without PULP_HAS_SKIA — offscreen GPU path is unavailable";
        return {};
    }
    std::vector<uint8_t> render_png(const PaintFn&) override {
        last_error_ = "HeadlessSurface: built without PULP_HAS_SKIA — PNG encode requires Skia";
        return {};
    }
    std::string last_error() const override { return last_error_; }

private:
    mutable std::string last_error_;
};

#endif  // PULP_HAS_SKIA

} // namespace

std::unique_ptr<HeadlessSurface> HeadlessSurface::create(
    const Config& config,
    std::string* last_error_out) {
    auto set_err = [&](const char* msg) {
        if (last_error_out) *last_error_out = msg;
    };

#if defined(PULP_HAS_SKIA)
    if (config.width == 0 || config.height == 0) {
        set_err("HeadlessSurface::create: width and height must be > 0");
        return nullptr;
    }

    auto gpu = GpuSurface::create_dawn();
    if (!gpu) {
        set_err("HeadlessSurface::create: GpuSurface::create_dawn() returned null (no Dawn backend)");
        return nullptr;
    }
    GpuSurface::Config gpu_config{};
    gpu_config.width = config.width;
    gpu_config.height = config.height;
    gpu_config.vsync = false;
    gpu_config.native_surface_handle = nullptr;  // offscreen / no presentation
    if (!gpu->initialize(gpu_config)) {
        set_err("HeadlessSurface::create: GpuSurface::initialize() failed (no native adapter?)");
        return nullptr;
    }

    SkiaSurface::Config skia_config{};
    skia_config.width = config.width;
    skia_config.height = config.height;
    skia_config.scale_factor = config.scale_factor;
    auto skia = SkiaSurface::create(*gpu, skia_config);
    if (!skia || !skia->is_available()) {
        set_err("HeadlessSurface::create: SkiaSurface::create() failed or reported unavailable");
        return nullptr;
    }

    if (last_error_out) last_error_out->clear();
    return std::make_unique<HeadlessSurfaceImpl>(std::move(gpu), std::move(skia), config);
#else
    (void)config;
    set_err("HeadlessSurface::create: build was configured without PULP_HAS_SKIA");
    return nullptr;
#endif
}

std::vector<uint8_t> HeadlessSurface::encode_png(const Rgba& rgba,
                                                 std::string* error_out) {
    auto set_err = [&](const char* msg) {
        if (error_out) *error_out = msg;
    };
    if (rgba.empty()) {
        set_err("HeadlessSurface::encode_png: empty RGBA input");
        return {};
    }
    const size_t need = static_cast<size_t>(rgba.width) * rgba.height * 4u;
    if (rgba.pixels.size() < need) {
        set_err("HeadlessSurface::encode_png: pixel buffer smaller than width*height*4");
        return {};
    }

#if defined(PULP_HAS_SKIA)
    auto info = SkImageInfo::Make(static_cast<int>(rgba.width),
                                  static_cast<int>(rgba.height),
                                  kRGBA_8888_SkColorType,
                                  kUnpremul_SkAlphaType);
    SkPixmap pixmap(info, rgba.pixels.data(),
                    static_cast<size_t>(rgba.width) * 4u);
    // SkPngEncoder offers a 2-arg pixmap overload that returns
    // `sk_sp<SkData>` (the 3-arg form takes an SkWStream* and returns
    // bool — we want the buffer back, so use the 2-arg form).
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png || png->size() == 0) {
        set_err("HeadlessSurface::encode_png: SkPngEncoder::Encode returned empty");
        return {};
    }
    std::vector<uint8_t> out(png->size());
    std::memcpy(out.data(), png->data(), png->size());
    if (error_out) error_out->clear();
    return out;
#else
    set_err("HeadlessSurface::encode_png: build was configured without PULP_HAS_SKIA");
    return {};
#endif
}

uint64_t HeadlessSurface::rgba_fingerprint(const Rgba& rgba) {
    // FNV-1a 64. Deterministic, no deps, fine for goldens-by-fingerprint.
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t h = kOffset;
    // Hash dimensions first so e.g. (10x20 = all-zero) and (20x10 = all-
    // zero) get different fingerprints.
    const uint32_t dims[2] = {rgba.width, rgba.height};
    const auto* dim_bytes = reinterpret_cast<const uint8_t*>(dims);
    for (size_t i = 0; i < sizeof(dims); ++i) {
        h ^= static_cast<uint64_t>(dim_bytes[i]);
        h *= kPrime;
    }
    for (uint8_t b : rgba.pixels) {
        h ^= static_cast<uint64_t>(b);
        h *= kPrime;
    }
    return h;
}

} // namespace pulp::render
