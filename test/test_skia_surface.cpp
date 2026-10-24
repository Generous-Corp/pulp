#include <catch2/catch_test_macros.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skp_capture.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/image_file_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/canvas/canvas.hpp>  // FillRule + Canvas path API (pulp #3656)
#include <pulp/view/svg_path_widget.hpp>  // gradient-stroke raster proof
#include "support/portable_env.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSerialProcs.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "support/dawn_transfer_call_counter.hpp"
#include "webgpu/webgpu_cpp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

using namespace pulp::render;

#ifdef PULP_HAS_SKIA
namespace {
// An unmet GPU precondition must SKIP out loud, never `return` — Catch2
// records a bare early return as a PASS, so a runner with no adapter would
// report this whole file green having asserted nothing about SkiaSurface.
constexpr const char* kNoGpu =
    "no GPU compute device available (Skia/Dawn not built, or no adapter)";
constexpr const char* kNoSkiaSurface =
    "SkiaSurface unavailable (Graphite context could not be created)";

// A presentable GpuSurface whose "drawable" is a texture the test owns, so the
// persistent-scene blit (which only runs when there is a drawable) can be
// driven headless and each frame's drawable read back afterwards. Everything
// else is the real Dawn device.
class TextureDrawableGpu final : public GpuSurface {
  public:
    explicit TextureDrawableGpu(GpuSurface& inner) : inner_(inner) {}
    void set_drawable(wgpu::Texture* texture) {
        drawable_ = texture;
    }

    bool initialize(const Config&) override {
        return inner_.is_initialized();
    }
    void resize(uint32_t w, uint32_t h) override {
        inner_.resize(w, h);
    }
    bool begin_frame() override {
        return inner_.begin_frame();
    }
    void end_frame() override {
        inner_.end_frame();
    }
    bool is_initialized() const override {
        return inner_.is_initialized();
    }
    bool has_surface() const override {
        return true;
    }
    uint32_t width() const override {
        return inner_.width();
    }
    uint32_t height() const override {
        return inner_.height();
    }
    void* dawn_device_handle() const override {
        return inner_.dawn_device_handle();
    }
    void* dawn_queue_handle() const override {
        return inner_.dawn_queue_handle();
    }
    void* dawn_instance_handle() const override {
        return inner_.dawn_instance_handle();
    }
    void* current_texture_handle() const override {
        return drawable_;
    }
    AdapterInfo adapter_info() const override {
        return inner_.adapter_info();
    }

  private:
    GpuSurface& inner_;
    wgpu::Texture* drawable_ = nullptr;
};

wgpu::Texture make_drawable_texture(GpuSurface& gpu, uint32_t w, uint32_t h) {
    auto* device = static_cast<wgpu::Device*>(gpu.dawn_device_handle());
    wgpu::TextureDescriptor desc{};
    desc.size = {w, h, 1};
    desc.format = wgpu::TextureFormat::RGBA8Unorm;
    desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                 wgpu::TextureUsage::CopySrc;
    return device->CreateTexture(&desc);
}

// Read a Graphite-backed surface back to RGBA8 through the context, after
// sending `recorder`'s pending work (the draws into `surface`) to the GPU.
bool read_graphite_surface(skgpu::graphite::Context& context, skgpu::graphite::Recorder& recorder,
                           SkSurface* surface, std::vector<uint8_t>& out) {
    if (auto recording = recorder.snap()) {
        skgpu::graphite::InsertRecordingInfo insert{};
        insert.fRecording = recording.get();
        if (context.insertRecording(insert) != skgpu::graphite::InsertStatus::kSuccess)
            return false;
    }
    // Graphite holds the callback context until the read completes, which
    // can be after the deadline below. The state is shared with the callback
    // so a late completion writes memory that is still alive.
    struct State {
        std::atomic<bool> done{false};
        bool ok = false;
        std::vector<uint8_t> pixels;
        int w = 0;
        int h = 0;
    };
    auto state = std::make_shared<State>();
    state->w = surface->width();
    state->h = surface->height();
    const auto info =
        SkImageInfo::Make(state->w, state->h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    context.asyncRescaleAndReadPixels(
        surface, info, SkIRect::MakeWH(state->w, state->h), SkImage::RescaleGamma::kSrc,
        SkImage::RescaleMode::kNearest,
        [](void* ctx, std::unique_ptr<const SkImage::AsyncReadResult> result) {
            std::unique_ptr<std::shared_ptr<State>> holder(
                static_cast<std::shared_ptr<State>*>(ctx));
            State& st = **holder;
            if (result && result->count() == 1) {
                const size_t row = static_cast<size_t>(st.w) * 4u;
                st.pixels.resize(row * static_cast<size_t>(st.h));
                const auto* src = static_cast<const uint8_t*>(result->data(0));
                for (int y = 0; y < st.h; ++y)
                    std::memcpy(st.pixels.data() + row * y, src + result->rowBytes(0) * y, row);
                st.ok = true;
            }
            st.done.store(true, std::memory_order_release);
        },
        new std::shared_ptr<State>(state));
    context.submit(skgpu::graphite::SyncToCpu::kYes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!state->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        context.checkAsyncWorkCompletion();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!state->done.load(std::memory_order_acquire) || !state->ok)
        return false;
    out = std::move(state->pixels);
    return true;
}

bool read_drawable(skgpu::graphite::Context& context, wgpu::Texture& texture,
                   std::vector<uint8_t>& out) {
    auto recorder = context.makeRecorder();
    if (!recorder)
        return false;
    auto surface = SkSurfaces::WrapBackendTexture(
        recorder.get(), skgpu::graphite::BackendTextures::MakeDawn(texture.Get()),
        kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
    if (!surface)
        return false;
    return read_graphite_surface(context, *recorder, surface.get(), out);
}

#if !defined(_WIN32)
// Captures what the process writes to stderr (where Skia's SkDebugf warnings
// go) between construction and text().
class StderrCapture {
  public:
    StderrCapture() {
        std::fflush(stderr);
        char path[] = "/tmp/pulp-skia-stderr-XXXXXX";
        fd_ = mkstemp(path);
        if (fd_ >= 0) {
            path_ = path;
            saved_ = dup(STDERR_FILENO);
            dup2(fd_, STDERR_FILENO);
        }
    }
    ~StderrCapture() {
        restore();
        if (fd_ >= 0) {
            close(fd_);
            std::remove(path_.c_str());
        }
    }
    bool active() const {
        return fd_ >= 0 && saved_ >= 0;
    }
    std::string text() {
        restore();
        std::ifstream in(path_);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

  private:
    void restore() {
        if (saved_ < 0)
            return;
        std::fflush(stderr);
        dup2(saved_, STDERR_FILENO);
        close(saved_);
        saved_ = -1;
    }
    int fd_ = -1;
    int saved_ = -1;
    std::string path_;
};
#endif

bool pixel_is(const std::vector<uint8_t>& px, uint32_t w, uint32_t x, uint32_t y, uint8_t r,
              uint8_t g, uint8_t b) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
    if (px.size() < i + 4)
        return false;
    auto close_to = [](uint8_t a, uint8_t e) { return std::abs(int(a) - int(e)) < 24; };
    return close_to(px[i], r) && close_to(px[i + 1], g) && close_to(px[i + 2], b);
}
}  // namespace
#endif

TEST_CASE("SkiaSurface requires initialized GpuSurface", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP(kNoGpu);

    // Don't initialize GpuSurface — SkiaSurface should fail gracefully
    SkiaSurface::Config config{};
    config.width = 400;
    config.height = 300;

    auto skia = SkiaSurface::create(*gpu, config);
    // Should be null because GpuSurface has no Dawn handles
    REQUIRE(skia == nullptr);
#else
    REQUIRE(true);  // Skia not compiled in
#endif
}

TEST_CASE("SkiaSurface uses shared GpuSurface device", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP(kNoGpu);

    GpuSurface::Config gpu_config{};
    gpu_config.width = 400;
    gpu_config.height = 300;

    if (!gpu->initialize(gpu_config)) SKIP(kNoGpu);

    SkiaSurface::Config config{};
    config.width = 400;
    config.height = 300;
    config.scale_factor = 1.0f;

    auto skia = SkiaSurface::create(*gpu, config);
    if (!skia) SKIP(kNoSkiaSurface);

    REQUIRE(skia->is_available());
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface offscreen frame cycle", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP(kNoGpu);

    GpuSurface::Config gpu_config{};
    gpu_config.width = 200;
    gpu_config.height = 150;

    if (!gpu->initialize(gpu_config)) SKIP(kNoGpu);

    SkiaSurface::Config config{};
    config.width = 200;
    config.height = 150;

    auto skia = SkiaSurface::create(*gpu, config);
    if (!skia || !skia->is_available()) SKIP(kNoSkiaSurface);

    // Frame cycle: GpuSurface brackets the frame, SkiaSurface draws
    REQUIRE(gpu->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);

    // Draw something
    canvas->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
    canvas->fill_rect(0, 0, 200, 150);

    skia->end_frame();  // submit Graphite recording
    gpu->end_frame();   // present (no-op in offscreen mode)
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface multiple frame cycles", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP(kNoGpu);

    GpuSurface::Config gpu_config{};
    gpu_config.width = 100;
    gpu_config.height = 100;

    if (!gpu->initialize(gpu_config)) SKIP(kNoGpu);

    auto skia = SkiaSurface::create(*gpu, {.width = 100, .height = 100});
    if (!skia || !skia->is_available()) SKIP(kNoSkiaSurface);

    // Multiple frames — verify no state leaks
    for (int i = 0; i < 5; ++i) {
        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->fill_rect(0, 0, 100, 100);
        skia->end_frame();
        gpu->end_frame();
    }
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface retains cacheable layers across frame canvases",
          "[render][skia][layer-cache]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP("Dawn GPU surface unavailable");

    GpuSurface::Config gpu_config{};
    gpu_config.width = 100;
    gpu_config.height = 100;
    if (!gpu->initialize(gpu_config))
        SKIP("Dawn adapter initialization failed");
    if (gpu->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    auto skia = SkiaSurface::create(*gpu, {.width = 100, .height = 100});
    if (!skia || !skia->is_available())
        SKIP("Skia Graphite surface unavailable");

    REQUIRE(gpu->begin_frame());
    auto* first_frame = skia->begin_frame();
    REQUIRE(first_frame != nullptr);
    REQUIRE(first_frame->supports(
        pulp::canvas::CanvasCapability::retained_layer_cache));

    first_frame->begin_layer({0.0f, 0.0f, 100.0f, 100.0f},
                             /*cacheable=*/true);
    first_frame->set_fill_color(
        pulp::canvas::Color::rgba8(255, 0, 0));
    first_frame->fill_rect(0.0f, 0.0f, 100.0f, 100.0f);
    const auto cached = first_frame->end_layer();
    REQUIRE(cached);
    REQUIRE(first_frame->layer_valid(cached));
    first_frame->draw_layer(cached);

    skia->end_frame();
    gpu->end_frame();

    REQUIRE(gpu->begin_frame());
    auto* second_frame = skia->begin_frame();
    REQUIRE(second_frame != nullptr);

    // SkiaSurface wraps each frame in a fresh SkiaCanvas. The renderer-owned
    // retained-layer store must keep both the handle and its SkImage alive.
    REQUIRE(second_frame->layer_valid(cached));
    second_frame->set_fill_color(
        pulp::canvas::Color::rgba8(0, 0, 255));
    second_frame->fill_rect(0.0f, 0.0f, 100.0f, 100.0f);
    second_frame->draw_layer(cached);

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(
        pixels, pixel_width, pixel_height));
    REQUIRE(pixel_width == 100);
    REQUIRE(pixel_height == 100);
    const size_t center =
        (static_cast<size_t>(50) * pixel_width + 50u) * 4u;
    REQUIRE(pixels.at(center) > 200);
    REQUIRE(pixels.at(center + 1) < 50);
    REQUIRE(pixels.at(center + 2) < 50);

    skia->end_frame();
    gpu->end_frame();

    // Moving the renderer to a new DPI retires the old-density texture.
    skia->resize(100, 100, 2.0f);
    REQUIRE(gpu->begin_frame());
    auto* scaled_frame = skia->begin_frame();
    REQUIRE(scaled_frame != nullptr);
    REQUIRE_FALSE(scaled_frame->layer_valid(cached));
    skia->end_frame();
    gpu->end_frame();
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface readback fails when Graphite rejects the flushed recording",
          "[render][skia][checked-submit]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu)
        SKIP(kNoGpu);

    GpuSurface::Config gpu_config{};
    gpu_config.width = 64;
    gpu_config.height = 64;
    if (!gpu->initialize(gpu_config))
        SKIP(kNoGpu);
    if (gpu->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    auto skia = SkiaSurface::create(*gpu, {.width = 64, .height = 64});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);

    // Restores the production path even when an assertion below throws.
    struct HookGuard {
        explicit HookGuard(SkiaSurface::InsertOutcomeHook hook) {
            SkiaSurface::set_insert_outcome_hook_for_testing(hook);
        }
        ~HookGuard() {
            SkiaSurface::set_insert_outcome_hook_for_testing(nullptr);
        }
    };

    auto draw_red_frame = [&]() {
        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
        canvas->fill_rect(0.0f, 0.0f, 64.0f, 64.0f);
    };

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;

    // Control: an accepted flush captures the frame and the frame succeeds.
    draw_red_frame();
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    REQUIRE(pixels.at((32u * pixel_width + 32u) * 4u) > 200);
    REQUIRE(skia->end_frame() == FrameOutcome::offscreen);
    REQUIRE(skia->wait_for_submitted_work());
    gpu->end_frame();

    // A rejected flush is a failed capture, and the frame it belonged to is a
    // failed frame rather than one whose work "already reached the GPU".
    {
        HookGuard reject([](bool) { return false; });
        draw_red_frame();
        REQUIRE_FALSE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
        REQUIRE(skia->end_frame() == FrameOutcome::failed);
        gpu->end_frame();
    }

    // Removing the hook restores normal captures on the same surface.
    draw_red_frame();
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    REQUIRE(skia->end_frame() == FrameOutcome::offscreen);
    gpu->end_frame();
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface persistent-scene blit presents each frame's own scene",
          "[render][skia][persistent-scene]") {
#ifdef PULP_HAS_SKIA
    constexpr uint32_t kSize = 64;
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = kSize, .height = kSize}))
        SKIP(kNoGpu);
    if (dawn->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    TextureDrawableGpu gpu(*dawn);
    auto skia = SkiaSurface::create(gpu, {.width = kSize, .height = kSize});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    REQUIRE(skia->set_persistent_scene(true));

    // Two drawables, as a swapchain rotates them: frame 1 presents into the
    // first, frame 2 into the second.
    wgpu::Texture drawable_1 = make_drawable_texture(*dawn, kSize, kSize);
    wgpu::Texture drawable_2 = make_drawable_texture(*dawn, kSize, kSize);
    REQUIRE(drawable_1);
    REQUIRE(drawable_2);

    // Frame 1 paints the whole scene red.
    gpu.set_drawable(&drawable_1);
    REQUIRE(gpu.begin_frame());
    auto* frame_1 = skia->begin_frame();
    REQUIRE(frame_1 != nullptr);
    frame_1->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
    frame_1->fill_rect(0.0f, 0.0f, 64.0f, 64.0f);
    REQUIRE(skia->end_frame() == FrameOutcome::presented);
    gpu.end_frame();

    // Frame 2 repaints only the left half, blue: the right half is retained.
    gpu.set_drawable(&drawable_2);
    REQUIRE(gpu.begin_frame());
    auto* frame_2 = skia->begin_frame();
    REQUIRE(frame_2 != nullptr);
    frame_2->set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255));
    frame_2->fill_rect(0.0f, 0.0f, 32.0f, 64.0f);
    REQUIRE(skia->end_frame() == FrameOutcome::presented);
    gpu.end_frame();
    REQUIRE(skia->wait_for_submitted_work());

    auto* context = skia->graphite_context();
    REQUIRE(context != nullptr);
    std::vector<uint8_t> px_1;
    std::vector<uint8_t> px_2;
    REQUIRE(read_drawable(*context, drawable_1, px_1));
    REQUIRE(read_drawable(*context, drawable_2, px_2));

    // Frame 1's drawable holds frame 1's scene: frame 2's later scene draws
    // did not reach back into a blit that shares the scene's texture. This
    // fails only if the blit leaves its own frame's recording (a cached view,
    // a deferred blit); it guards that ordering rather than probing a hazard
    // the current code has.
    CHECK(pixel_is(px_1, kSize, 16, 32, 255, 0, 0));
    CHECK(pixel_is(px_1, kSize, 48, 32, 255, 0, 0));
    // Frame 2's drawable shows frame 2: its repaint plus the retained half.
    CHECK(pixel_is(px_2, kSize, 16, 32, 0, 0, 255));
    CHECK(pixel_is(px_2, kSize, 48, 32, 255, 0, 0));
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface readback fallback keeps the scene blit free of snapshot warnings",
          "[render][skia][persistent-scene]") {
#ifdef PULP_HAS_SKIA
    constexpr uint32_t kSize = 64;
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = kSize, .height = kSize}))
        SKIP(kNoGpu);
    if (dawn->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    TextureDrawableGpu gpu(*dawn);
    auto skia = SkiaSurface::create(gpu, {.width = kSize, .height = kSize});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    REQUIRE(skia->set_persistent_scene(true));
    wgpu::Texture drawable = make_drawable_texture(*dawn, kSize, kSize);
    REQUIRE(drawable);
    gpu.set_drawable(&drawable);

    // Restores the asynchronous readback even when an assertion throws.
    struct AsyncReadbackOff {
        AsyncReadbackOff() {
            SkiaSurface::set_async_readback_for_testing(false);
        }
        ~AsyncReadbackOff() {
            SkiaSurface::set_async_readback_for_testing(true);
        }
    };

    auto paint = [](pulp::canvas::Canvas& canvas, pulp::canvas::Color color, float width) {
        canvas.set_fill_color(color);
        canvas.fill_rect(0.0f, 0.0f, width, 64.0f);
    };

    // Frame 1 paints the scene red and blits it.
    REQUIRE(gpu.begin_frame());
    auto* frame_1 = skia->begin_frame();
    REQUIRE(frame_1 != nullptr);
    paint(*frame_1, pulp::canvas::Color::rgba8(255, 0, 0), 64.0f);
    REQUIRE(skia->end_frame() == FrameOutcome::presented);
    gpu.end_frame();
    REQUIRE(skia->wait_for_submitted_work());

#if !defined(_WIN32)
    StderrCapture captured;
    REQUIRE(captured.active());
#endif

    // Frame 2 repaints the left half blue and reads the scene back through
    // the fallback a timed-out readback takes, then blits as usual. Frame 3
    // blits again, which is where a snapshot taken by the fallback would make
    // Graphite warn about mixing the two image APIs.
    std::vector<uint8_t> px;
    uint32_t pw = 0;
    uint32_t ph = 0;
    bool fallback_read = false;
    {
        AsyncReadbackOff fallback;
        REQUIRE(gpu.begin_frame());
        auto* frame_2 = skia->begin_frame();
        REQUIRE(frame_2 != nullptr);
        paint(*frame_2, pulp::canvas::Color::rgba8(0, 0, 255), 32.0f);
        fallback_read = skia->read_current_rgba(px, pw, ph);
        REQUIRE(frame_reached_output(skia->end_frame()));
        gpu.end_frame();
    }
    REQUIRE(gpu.begin_frame());
    auto* frame_3 = skia->begin_frame();
    REQUIRE(frame_3 != nullptr);
    paint(*frame_3, pulp::canvas::Color::rgba8(0, 255, 0), 8.0f);
    REQUIRE(skia->end_frame() == FrameOutcome::presented);
    gpu.end_frame();
    REQUIRE(skia->wait_for_submitted_work());

#if !defined(_WIN32)
    const std::string log = captured.text();
    INFO("stderr during the fallback frames: " << log);
    CHECK(log.find("Intermingling makeImageSnapshot and asImage") == std::string::npos);
#endif

    // On Graphite the synchronous fallback cannot read a texture-backed image
    // without a context, so it reports failure rather than handing back
    // pixels; it must still leave the scene and the blits intact.
    INFO("fallback readback succeeded: " << fallback_read);

    // The drawable shows frame 3: the blits after the fallback still work.
    std::vector<uint8_t> shown;
    auto* context = skia->graphite_context();
    REQUIRE(context != nullptr);
    REQUIRE(read_drawable(*context, drawable, shown));
    CHECK(pixel_is(shown, kSize, 4, 32, 0, 255, 0));
    CHECK(pixel_is(shown, kSize, 16, 32, 0, 0, 255));
    CHECK(pixel_is(shown, kSize, 48, 32, 255, 0, 0));
#else
    REQUIRE(true);
#endif
}

TEST_CASE("Graphite AsImage view of a surface sees later draws into it",
          "[render][skia][persistent-scene]") {
#ifdef PULP_HAS_SKIA
    // The control for the two-frame test above: a view kept past a write to
    // its surface DOES change, so that test's frame-1 check can fail. This
    // shows aliasing inside one recording; across recordings the order is the
    // queue's.
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = 16, .height = 16}))
        SKIP(kNoGpu);
    if (dawn->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");
    auto skia = SkiaSurface::create(*dawn, {.width = 16, .height = 16});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    auto* context = skia->graphite_context();
    REQUIRE(context != nullptr);
    auto recorder = context->makeRecorder();
    REQUIRE(recorder);

    const auto info = SkImageInfo::MakeN32Premul(16, 16);
    auto source = SkSurfaces::RenderTarget(recorder.get(), info);
    auto target = SkSurfaces::RenderTarget(recorder.get(), info);
    REQUIRE(source);
    REQUIRE(target);

    source->getCanvas()->clear(SK_ColorRED);
    sk_sp<SkImage> view = SkSurfaces::AsImage(source);
    sk_sp<SkImage> copy = source->makeImageSnapshot();
    REQUIRE(view);
    REQUIRE(copy);
    source->getCanvas()->clear(SK_ColorBLUE);

    std::vector<uint8_t> px;
    target->getCanvas()->drawImage(view, 0, 0);
    REQUIRE(read_graphite_surface(*context, *recorder, target.get(), px));
    CHECK(pixel_is(px, 16, 8, 8, 0, 0, 255));

    target->getCanvas()->drawImage(copy, 0, 0);
    REQUIRE(read_graphite_surface(*context, *recorder, target.get(), px));
    CHECK(pixel_is(px, 16, 8, 8, 255, 0, 0));
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface persistent-scene frames do not copy the scene",
          "[render][skia][persistent-scene]") {
#ifdef PULP_HAS_SKIA
    constexpr uint32_t kSize = 256;
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = kSize, .height = kSize}))
        SKIP(kNoGpu);
    if (dawn->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    TextureDrawableGpu gpu(*dawn);
    auto skia = SkiaSurface::create(gpu, {.width = kSize, .height = kSize});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    REQUIRE(skia->set_persistent_scene(true));
    wgpu::Texture drawable = make_drawable_texture(*dawn, kSize, kSize);
    REQUIRE(drawable);
    gpu.set_drawable(&drawable);
    // Installed after GpuSurface::initialize(), which sets the baseline proc
    // table itself and would replace an interposer installed before it. The
    // table this restores is that same baseline.
    pulp::test::DawnTransferCallCounter work;

    auto frame = [&](int i) {
        REQUIRE(gpu.begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->set_fill_color(pulp::canvas::Color::rgba8(static_cast<uint8_t>(i * 4), 0, 0));
        canvas->fill_rect(0.0f, 0.0f, 16.0f, 16.0f); // a small damaged rect
        REQUIRE(skia->end_frame() == FrameOutcome::presented);
        gpu.end_frame();
        REQUIRE(skia->wait_for_submitted_work());
    };

    // The first frames pay for the scene, pipelines and atlases.
    for (int i = 0; i < 3; ++i)
        frame(i);
    const auto warm = work.snapshot();
    // Control: the counter sees Graphite's allocations.
    REQUIRE(warm.device_create_texture_calls > 0);
    constexpr int kFrames = 60;
    for (int i = 0; i < kFrames; ++i)
        frame(i);
    const auto steady = work.snapshot();
    const auto copies = steady.copy_texture_to_texture_calls - warm.copy_texture_to_texture_calls;
    INFO("over " << kFrames << " steady frames: " << copies << " texture copies, "
                 << steady.device_create_texture_calls - warm.device_create_texture_calls
                 << " textures created (first 3 frames: " << warm.copy_texture_to_texture_calls
                 << " copies, " << warm.device_create_texture_calls << " textures)");

    // A blit through a snapshot copies the whole scene every frame; the
    // shared-texture blit draws straight from the scene's own texture. Texture
    // creation cannot tell the two apart, because Graphite recycles the
    // snapshot's copy from its scratch pool.
    CHECK(copies == 0);
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaCanvas Graphite layer seal shares the layer texture",
          "[render][skia][layer-cache]") {
#ifdef PULP_HAS_SKIA
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = 16, .height = 16}))
        SKIP(kNoGpu);
    if (dawn->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");
    auto skia = SkiaSurface::create(*dawn, {.width = 16, .height = 16});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    auto* context = skia->graphite_context();
    REQUIRE(context != nullptr);
    auto recorder = context->makeRecorder();
    REQUIRE(recorder);

    constexpr int kTarget = 128;
    constexpr int kLayer = 128;
    auto target =
        SkSurfaces::RenderTarget(recorder.get(), SkImageInfo::MakeN32Premul(kTarget, kTarget));
    REQUIRE(target);
    auto store = pulp::canvas::SkiaCanvas::create_retained_layer_store();
    pulp::canvas::SkiaCanvas canvas(target->getCanvas(), recorder.get(), store);
    REQUIRE(canvas.supports(pulp::canvas::CanvasCapability::retained_layer_cache));

    auto seal = [&](pulp::canvas::Color color) {
        canvas.begin_layer({0.0f, 0.0f, float(kLayer), float(kLayer)}, /*cacheable=*/true);
        canvas.set_fill_color(color);
        canvas.fill_rect(0.0f, 0.0f, float(kLayer), float(kLayer));
        return canvas.end_layer();
    };

    // Each retained layer costs one texture: the one it was recorded into. A
    // seal that copies costs about a second one per seal (15 textures for 8
    // seals, measured). The bound leaves room for a backend that adds a few
    // attachments of its own. Snapping the recording is what makes Graphite
    // instantiate the textures. The counter goes in after
    // GpuSurface::initialize(), which installs the baseline proc table itself.
    const auto warmup = seal(pulp::canvas::Color::rgba8(0, 255, 0));
    REQUIRE(warmup);
    REQUIRE(recorder->snap());
    pulp::test::DawnTransferCallCounter work;
    constexpr int kSeals = 8;
    std::vector<pulp::canvas::Canvas::LayerHandle> kept;
    for (int i = 0; i < kSeals; ++i) {
        kept.push_back(seal(pulp::canvas::Color::rgba8(0, 255, 0)));
        REQUIRE(kept.back());
    }
    REQUIRE(recorder->snap());
    const auto textures = work.snapshot().device_create_texture_calls;
    INFO("textures created by " << kSeals << " retained seals: " << textures);
    CHECK(textures >= static_cast<std::uint64_t>(kSeals));
    CHECK(textures < static_cast<std::uint64_t>(kSeals + kSeals / 2));

    // Sanity check, true of the copying seal too: a layer sealed before a
    // later one is recorded still draws its own content.
    const auto red = seal(pulp::canvas::Color::rgba8(255, 0, 0));
    REQUIRE(red);
    const auto blue = seal(pulp::canvas::Color::rgba8(0, 0, 255));
    REQUIRE(blue);
    canvas.draw_layer(red);
    std::vector<uint8_t> px;
    REQUIRE(read_graphite_surface(*context, *recorder, target.get(), px));
    CHECK(pixel_is(px, kTarget, 64, 64, 255, 0, 0));
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaCanvas retained stores isolate handles and open layers",
          "[render][skia][layer-cache]") {
#ifdef PULP_HAS_SKIA
    const auto info = SkImageInfo::MakeN32Premul(32, 32);
    auto surface_a = SkSurfaces::Raster(info);
    auto surface_b = SkSurfaces::Raster(info);
    REQUIRE(surface_a);
    REQUIRE(surface_b);

    auto store_a =
        pulp::canvas::SkiaCanvas::create_retained_layer_store();
    auto store_b =
        pulp::canvas::SkiaCanvas::create_retained_layer_store();
    pulp::canvas::SkiaCanvas canvas_a(
        surface_a->getCanvas(), nullptr, store_a);
    pulp::canvas::SkiaCanvas canvas_b(
        surface_b->getCanvas(), nullptr, store_b);

    canvas_a.begin_layer({0, 0, 16, 16}, true);
    canvas_a.fill_rect(0, 0, 16, 16);
    const auto layer_a = canvas_a.end_layer();
    canvas_b.begin_layer({0, 0, 16, 16}, true);
    canvas_b.fill_rect(0, 0, 16, 16);
    const auto layer_b = canvas_b.end_layer();

    REQUIRE(layer_a);
    REQUIRE(layer_b);
    REQUIRE(layer_a != layer_b);
    REQUIRE(canvas_a.layer_valid(layer_a));
    REQUIRE_FALSE(canvas_a.layer_valid(layer_b));
    REQUIRE_FALSE(canvas_b.layer_valid(layer_a));

    // Invalidating an open ancestor cancels it without releasing the surface
    // that the nested layer's previous_canvas still points into.
    const auto outer = canvas_a.begin_layer({0, 0, 32, 32}, true);
    const auto inner = canvas_a.begin_layer({0, 0, 8, 8}, true);
    canvas_a.invalidate_layer(outer);
    const auto sealed_inner = canvas_a.end_layer();
    REQUIRE(sealed_inner == inner);
    canvas_a.draw_layer(sealed_inner);
    REQUIRE_FALSE(canvas_a.end_layer());
    REQUIRE_FALSE(canvas_a.layer_valid(outer));

    pulp::canvas::Canvas::LayerHandle abandoned;
    {
        pulp::canvas::SkiaCanvas unbalanced(
            surface_a->getCanvas(), nullptr, store_a);
        abandoned = unbalanced.begin_layer({0, 0, 4, 4}, true);
    }
    pulp::canvas::SkiaCanvas after_unwind(
        surface_a->getCanvas(), nullptr, store_a);
    REQUIRE_FALSE(after_unwind.layer_valid(abandoned));
#else
    REQUIRE(true);
#endif
}

TEST_CASE("GpuIdleTrimSchedule is due once per interval", "[render][gpu-trim]") {
    using Clock = GpuIdleTrimSchedule::Clock;
    const Clock::time_point t0{std::chrono::seconds(100)};
    GpuIdleTrimSchedule schedule(t0);
    CHECK_FALSE(schedule.due(t0));
    CHECK_FALSE(schedule.due(t0 + GpuIdleTrimSchedule::kInterval - std::chrono::nanoseconds(1)));
    CHECK(schedule.due(t0 + GpuIdleTrimSchedule::kInterval));
    // The interval restarts at the trim, not at construction.
    CHECK_FALSE(schedule.due(t0 + GpuIdleTrimSchedule::kInterval + std::chrono::milliseconds(999)));
    CHECK(schedule.due(t0 + 2 * GpuIdleTrimSchedule::kInterval));
    CHECK(schedule.trims() == 2);
}

#ifdef PULP_HAS_SKIA
namespace {
constexpr std::size_t kMiB = 1024 * 1024;

// Writes `count` distinct solid-colour PNGs, `side` pixels square, into a
// fresh directory. Each decodes to a `side * side * 4`-byte texture.
std::vector<std::string> write_trim_images(const std::string& tag, int count, int side) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("pulp-gpu-trim-" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<std::string> paths;
    for (int i = 0; i < count; ++i) {
        SkBitmap bitmap;
        bitmap.allocN32Pixels(side, side);
        bitmap.eraseColor(SkColorSetARGB(255, static_cast<U8CPU>(10 + 30 * i),
                                         static_cast<U8CPU>(240 - 30 * i),
                                         static_cast<U8CPU>(7 * i)));
        const fs::path path = dir / ("image-" + std::to_string(i) + ".png");
        SkFILEWStream out(path.string().c_str());
        if (!out.isValid() || !SkPngEncoder::Encode(&out, bitmap.pixmap(), {}))
            return {};
        paths.push_back(path.string());
    }
    return paths;
}

// One frame that draws every image in `paths`, then waits for the GPU so the
// frame's recording no longer references anything it drew.
bool draw_trim_frame(SkiaSurface& skia, const std::vector<std::string>& paths) {
    auto* canvas = skia.begin_frame();
    if (!canvas)
        return false;
    bool drawn = true;
    for (const auto& path : paths)
        drawn = canvas->draw_image_from_file(path, 0.0f, 0.0f, 32.0f, 32.0f) && drawn;
    canvas->set_fill_color(pulp::canvas::Color::rgba8(40, 40, 40));
    canvas->fill_rect(40.0f, 40.0f, 8.0f, 8.0f);
    const bool reached = frame_reached_output(skia.end_frame());
    skia.wait_for_submitted_work();
    return drawn && reached;
}

std::atomic<std::int64_t> g_fake_trim_clock_ns{0};

GpuIdleTrimSchedule::Clock::time_point fake_trim_clock() {
    return GpuIdleTrimSchedule::Clock::time_point(
        std::chrono::nanoseconds(g_fake_trim_clock_ns.load(std::memory_order_relaxed)));
}

struct TrimClockGuard {
    TrimClockGuard() {
        SkiaSurface::set_trim_clock_for_testing(&fake_trim_clock);
    }
    ~TrimClockGuard() {
        SkiaSurface::set_trim_clock_for_testing(nullptr);
    }
};

// The image cache is process-wide, so a failed REQUIRE must not leave a later
// test running with this one's capacity.
struct ImageCacheCapacityGuard {
    explicit ImageCacheCapacityGuard(std::size_t capacity) {
        pulp::canvas::ImageFileCache::instance().set_capacity(capacity);
    }
    ~ImageCacheCapacityGuard() {
        auto& cache = pulp::canvas::ImageFileCache::instance();
        cache.set_capacity(32);
        cache.clear();
    }
};
} // namespace
#endif

TEST_CASE("SkiaSurface trim everything returns released image textures to the device",
          "[render][skia][gpu-trim]") {
#ifdef PULP_HAS_SKIA
    constexpr uint32_t kSize = 256;
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = kSize, .height = kSize}))
        SKIP(kNoGpu);
    auto skia = SkiaSurface::create(*dawn, {.width = kSize, .height = kSize});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    const auto paths = write_trim_images("everything", 8, 1024);
    REQUIRE(paths.size() == 8);

    auto& cache = pulp::canvas::ImageFileCache::instance();
    cache.clear();
    REQUIRE(draw_trim_frame(*skia, paths));
    const auto drawn = skia->gpu_cache_bytes();
    // The cache held the only references to the eight textures.
    cache.clear();
    REQUIRE(draw_trim_frame(*skia, {}));
    const auto released = skia->gpu_cache_bytes();
    INFO("drawn: budgeted " << drawn.budgeted << " purgeable " << drawn.purgeable
                            << "; released: budgeted " << released.budgeted << " purgeable "
                            << released.purgeable);
    // Control: the released textures are still resident and counted.
    REQUIRE(released.purgeable >= 24 * kMiB);

    skia->trim_gpu_resources(SkiaSurface::TrimLevel::everything());
    const auto trimmed = skia->gpu_cache_bytes();
    INFO("trimmed: budgeted " << trimmed.budgeted << " purgeable " << trimmed.purgeable);
    CHECK(trimmed.purgeable < 4 * kMiB);
    CHECK(trimmed.budgeted + 24 * kMiB <= released.budgeted);
    // The surface still draws after a full trim.
    CHECK(draw_trim_frame(*skia, {}));
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface idle trim frees textures unused for two seconds and keeps the drawn one",
          "[render][skia][gpu-trim]") {
#ifdef PULP_HAS_SKIA
    constexpr uint32_t kSize = 256;
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = kSize, .height = kSize}))
        SKIP(kNoGpu);
    auto skia = SkiaSurface::create(*dawn, {.width = kSize, .height = kSize});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);
    // A differs in size from the released images, so its texture can never
    // be a recycled one of theirs.
    const auto released_paths = write_trim_images("idle-released", 7, 1024);
    const auto kept_paths = write_trim_images("idle-kept", 1, 512);
    REQUIRE(released_paths.size() == 7);
    REQUIRE(kept_paths.size() == 1);

    auto& cache = pulp::canvas::ImageFileCache::instance();
    cache.clear();
    REQUIRE(draw_trim_frame(*skia, released_paths));
    // With room for one entry, caching A evicts the other seven.
    ImageCacheCapacityGuard capacity(1);
    REQUIRE(draw_trim_frame(*skia, kept_paths));
    REQUIRE(draw_trim_frame(*skia, kept_paths));
    const auto released = skia->gpu_cache_bytes();
    INFO("released: budgeted " << released.budgeted << " purgeable " << released.purgeable);
    REQUIRE(released.purgeable >= 24 * kMiB);

    // Control: an idle window longer than the time since release frees none
    // of it, so the level is honoured rather than treated as everything.
    skia->trim_gpu_resources(SkiaSurface::TrimLevel::unused_for(std::chrono::minutes(10)));
    REQUIRE(skia->gpu_cache_bytes().purgeable >= 24 * kMiB);

    const auto builds = cache.stats().builds;
    const auto trims = skia->idle_trim_count();
    pulp::test::DawnTransferCallCounter work;
    const auto warm = work.snapshot();

    // Draw only A at frame pace until end_frame()'s own idle trim has run
    // past kUnusedAge. Skia stamps resources with the real clock, so this
    // takes real time: kUnusedAge plus at most one kInterval.
    const auto deadline = std::chrono::steady_clock::now() + GpuIdleTrimSchedule::kUnusedAge +
                          2 * GpuIdleTrimSchedule::kInterval + std::chrono::seconds(1);
    bool freed = false;
    int frames = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        REQUIRE(draw_trim_frame(*skia, kept_paths));
        ++frames;
        if (skia->gpu_cache_bytes().purgeable + 24 * kMiB <= released.purgeable) {
            freed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const auto after = skia->gpu_cache_bytes();
    const auto steady = work.snapshot();
    INFO("after " << frames << " frames: budgeted " << after.budgeted << " purgeable "
                  << after.purgeable << ", idle trims " << skia->idle_trim_count() - trims
                  << ", textures created "
                  << steady.device_create_texture_calls - warm.device_create_texture_calls);
    CHECK(freed);
    CHECK(skia->idle_trim_count() > trims);
    // A stayed resident: drawing it again neither rebuilt nor re-uploaded it,
    // and the per-frame scratch targets the idle trim spared were reused.
    CHECK(cache.stats().builds == builds);
    CHECK(steady.device_create_texture_calls == warm.device_create_texture_calls);
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface idle trim runs once per second of frames", "[render][skia][gpu-trim]") {
#ifdef PULP_HAS_SKIA
    constexpr uint32_t kSize = 64;
    auto dawn = GpuSurface::create_dawn();
    if (!dawn)
        SKIP(kNoGpu);
    if (!dawn->initialize({.width = kSize, .height = kSize}))
        SKIP(kNoGpu);
    g_fake_trim_clock_ns.store(std::chrono::nanoseconds(std::chrono::hours(1)).count());
    TrimClockGuard clock;
    auto skia = SkiaSurface::create(*dawn, {.width = kSize, .height = kSize});
    if (!skia || !skia->is_available())
        SKIP(kNoSkiaSurface);

    // 120 frames spread over exactly one second of the injected clock.
    constexpr std::int64_t kStepNs = 1'000'000'000 / 120 + 1;
    auto frame = [&] {
        g_fake_trim_clock_ns.fetch_add(kStepNs, std::memory_order_relaxed);
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->set_fill_color(pulp::canvas::Color::rgba8(200, 0, 0));
        canvas->fill_rect(0.0f, 0.0f, 8.0f, 8.0f);
        REQUIRE(frame_reached_output(skia->end_frame()));
    };
    for (int i = 0; i < 119; ++i)
        frame();
    CHECK(skia->idle_trim_count() == 0);
    frame();
    CHECK(skia->idle_trim_count() == 1);
    for (int i = 0; i < 120; ++i)
        frame();
    CHECK(skia->idle_trim_count() == 2);
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface resize", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP(kNoGpu);

    GpuSurface::Config gpu_config{};
    gpu_config.width = 200;
    gpu_config.height = 200;

    if (!gpu->initialize(gpu_config)) SKIP(kNoGpu);

    auto skia = SkiaSurface::create(*gpu, {.width = 200, .height = 200});
    if (!skia || !skia->is_available()) SKIP(kNoSkiaSurface);

    // Resize both GpuSurface and SkiaSurface
    gpu->resize(400, 300);
    skia->resize(400, 300);

    // Should still work after resize
    REQUIRE(gpu->begin_frame());
    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    canvas->fill_rect(0, 0, 400, 300);
    skia->end_frame();
    gpu->end_frame();
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface null without Skia", "[render][skia]") {
#ifndef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (gpu) {
        auto skia = SkiaSurface::create(*gpu, {});
        REQUIRE(skia == nullptr);
    }
#else
    REQUIRE(true);  // Skia is available, tested elsewhere
#endif
}

// ---------------------------------------------------------------------------
// SkPicture (.skp) serialization on the Graphite backend
//
// The frame-capture path writes a `.skp` artifact that the Skia team's
// own `skiadebugger` can replay. Pulp renders through the Graphite
// backend (skgpu::graphite::Context) — `.skp` / SkPicture is the legacy
// SkPicture serialization format, so we must verify the two compose.
//
// Verdict from these tests: `SkPicture` is backend-independent. An
// `SkPictureRecorder` records into a backend-agnostic `SkRecord`; nothing
// in the Graphite headers references `SkPicture`. So `serialize()` /
// `MakeFromData()` round-trips cleanly regardless of which GPU backend the
// process is running. The single caveat is documented in SkPicture.h: the
// default serializer encodes embedded `SkImage`s as nullptr unless the
// caller supplies `SkSerialProcs::fImageProc`. That is format policy, not a
// Graphite limitation: callers must set fImageProc to capture frames that
// embed images. These tests pin both facts.
// ---------------------------------------------------------------------------
#ifdef PULP_HAS_SKIA

namespace {

// Record a small, deterministic vector scene (no embedded images) into an
// SkPicture. SkPictureRecorder is GPU-backend-agnostic by construction.
sk_sp<SkPicture> record_vector_scene() {
    SkPictureRecorder recorder;
    SkCanvas* rec = recorder.beginRecording(SkRect::MakeWH(64.0f, 48.0f));
    REQUIRE(rec != nullptr);

    SkPaint red;
    red.setColor(SK_ColorRED);
    red.setAntiAlias(true);
    rec->drawRect(SkRect::MakeXYWH(4.0f, 4.0f, 40.0f, 24.0f), red);

    SkPaint blue;
    blue.setColor(SK_ColorBLUE);
    rec->drawCircle(32.0f, 24.0f, 12.0f, blue);

    return recorder.finishRecordingAsPicture();
}

// Replay a picture into a fresh raster surface (pre-cleared to black) and
// read back the top-left pixel. cullRect equality only proves the picture
// *structure* survived serialization; replaying and sampling a pixel is
// what proves an embedded image's payload actually round-tripped.
SkColor replay_top_left(const sk_sp<SkPicture>& picture,
                        const SkImageInfo& info) {
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorBLACK);
    surface->getCanvas()->drawPicture(picture.get());

    SkBitmap bm;
    REQUIRE(bm.tryAllocPixels(info));
    REQUIRE(surface->readPixels(bm, 0, 0));
    return bm.getColor(0, 0);
}

// Replay a picture into a fresh black raster surface and read back the
// pixel at (x, y). Used by the fill-rule raster proof below.
SkColor replay_pixel_at(const sk_sp<SkPicture>& picture,
                        const SkImageInfo& info, int x, int y) {
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorBLACK);
    surface->getCanvas()->drawPicture(picture.get());

    SkBitmap bm;
    REQUIRE(bm.tryAllocPixels(info));
    REQUIRE(surface->readPixels(bm, 0, 0));
    return bm.getColor(x, y);
}

}  // namespace

TEST_CASE("SkPicture round-trips through serialize/MakeFromData", "[render][skia][skp]") {
    sk_sp<SkPicture> picture = record_vector_scene();
    REQUIRE(picture != nullptr);

    const SkRect bounds = picture->cullRect();
    REQUIRE(bounds.width() == 64.0f);
    REQUIRE(bounds.height() == 48.0f);

    // Serialize to an in-memory .skp blob.
    sk_sp<SkData> blob = picture->serialize();
    REQUIRE(blob != nullptr);
    REQUIRE(blob->size() > 0);

    // The blob carries the documented .skp file signature ("skiapict"),
    // which is what skiadebugger keys on to recognize the artifact.
    REQUIRE(blob->size() >= 8);
    REQUIRE(std::memcmp(blob->data(), "skiapict", 8) == 0);

    // Deserialize and assert the round-trip preserved the recording.
    sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob.get());
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == bounds);
    REQUIRE(restored->approximateOpCount() == picture->approximateOpCount());
    REQUIRE(restored->approximateOpCount() > 0);
}

TEST_CASE("SkPicture round-trips through an SkStream", "[render][skia][skp]") {
    sk_sp<SkPicture> picture = record_vector_scene();
    REQUIRE(picture != nullptr);

    // serialize(SkWStream*) is the file-write path.
    SkDynamicMemoryWStream out;
    picture->serialize(&out);
    REQUIRE(out.bytesWritten() > 0);

    sk_sp<SkData> blob = out.detachAsData();
    REQUIRE(blob != nullptr);

    SkMemoryStream in(blob);
    sk_sp<SkPicture> restored = SkPicture::MakeFromStream(&in);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == picture->cullRect());
    REQUIRE(restored->approximateOpCount() == picture->approximateOpCount());
}

TEST_CASE("SkPicture serialization is independent of any GPU context", "[render][skia][skp]") {
    // SkPicture must work without — and regardless of — a live Graphite
    // context. A picture recorded and serialized with no GPU context
    // whatsoever still round-trips, which is exactly why it is safe on the
    // Graphite backend (Graphite never touches the SkPicture pipeline).
    sk_sp<SkPicture> picture = record_vector_scene();
    sk_sp<SkData> blob = picture->serialize();
    REQUIRE(blob != nullptr);

    sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob->data(), blob->size());
    REQUIRE(restored != nullptr);
    REQUIRE(restored->uniqueID() != 0);
    REQUIRE(restored->approximateOpCount() == picture->approximateOpCount());
}

TEST_CASE("SkPicture with embedded image needs fImageProc to round-trip pixels",
          "[render][skia][skp]") {
    // SkPicture.h documents: "The default behavior for serializing SkImages
    // is to encode a nullptr." A captured frame that embeds images must
    // supply SkSerialProcs so the image survives the round trip. This test
    // pins that contract so frame capture does not silently ship null-image
    // captures.

    // A tiny raster image (the kind a real frame would embed; Graphite frames
    // upload raster sources, so this mirrors the captured-frame case).
    SkImageInfo info = SkImageInfo::MakeN32Premul(8, 8);
    sk_sp<SkSurface> raster = SkSurfaces::Raster(info);
    REQUIRE(raster != nullptr);
    raster->getCanvas()->clear(SK_ColorGREEN);
    sk_sp<SkImage> image = raster->makeImageSnapshot();
    REQUIRE(image != nullptr);

    SkPictureRecorder recorder;
    SkCanvas* rec = recorder.beginRecording(SkRect::MakeWH(8.0f, 8.0f));
    rec->drawImage(image, 0.0f, 0.0f);
    sk_sp<SkPicture> picture = recorder.finishRecordingAsPicture();
    REQUIRE(picture != nullptr);

    // (a) Default serialization: the picture structure survives, the
    //     embedded image is dropped to null. Still a valid .skp.
    {
        sk_sp<SkData> blob = picture->serialize();
        REQUIRE(blob != nullptr);
        sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob.get());
        REQUIRE(restored != nullptr);
        REQUIRE(restored->cullRect() == picture->cullRect());

        // The image payload did NOT survive: replaying the restored
        // picture draws nothing where the image was, so the surface
        // keeps its black clear color. This is the contrast that makes
        // the (b) pixel assertion below meaningful.
        REQUIRE(replay_top_left(restored, info) == SK_ColorBLACK);
    }

    // (b) With matching SkSerialProcs/SkDeserialProcs fImageProc set, the
    //     embedded image round-trips through serialize + MakeFromData.
    {
        SkSerialProcs sprocs;
        // Match the SkSerialImageProc return type selected by the packaged
        // Skia headers.
        sprocs.fImageProc = [](SkImage* img, void*) -> SkSerialReturnType {
            return SkPngEncoder::Encode(nullptr, img, SkPngEncoder::Options{});
        };
        sk_sp<SkData> blob = picture->serialize(&sprocs);
        REQUIRE(blob != nullptr);

        SkDeserialProcs dprocs;
        dprocs.fImageProc = [](const void* data, size_t length,
                               std::optional<SkAlphaType> alpha_type, void*) -> sk_sp<SkImage> {
            return SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(data, length),
                                                     alpha_type);
        };
        sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob.get(), &dprocs);
        REQUIRE(restored != nullptr);
        REQUIRE(restored->cullRect() == picture->cullRect());

        // The image payload survived: replaying the restored picture
        // reproduces the original green image. cullRect equality alone
        // would still pass if SkPngEncoder::Encode had returned null and
        // the image deserialized to nothing — this pixel check is what
        // actually proves fImageProc preserved the pixels.
        REQUIRE(replay_top_left(restored, info) == SK_ColorGREEN);
    }
}

#endif  // PULP_HAS_SKIA

// ---------------------------------------------------------------------------
// SkpFrameCapture — `.skp` frame-capture API (core/render)
//
// These exercise the public capture surface in
// core/render/include/pulp/render/skp_capture.hpp: record a frame's
// draw ops through the capture's pulp::canvas::Canvas, serialize a
// `.skp` artifact, and prove the round trip — including the load-bearing
// embedded-image-pixel assertion (cullRect equality alone is insufficient).
// The capture path is GPU-backend-agnostic and
// needs no live GPU context, so these run on the CI matrix even without
// a GPU adapter.
// ---------------------------------------------------------------------------

#ifdef PULP_HAS_SKIA

namespace {

// Deserialize-side image proc — the replay-side half of the contract
// SkpFrameCapture's fImageProc establishes. Decodes the PNG payload
// SkpFrameCapture wrote back into an SkImage.
SkDeserialProcs skp_deserial_procs() {
    SkDeserialProcs procs;
    procs.fImageProc = [](const void* data, size_t length, std::optional<SkAlphaType> alpha_type,
                          void*) -> sk_sp<SkImage> {
        return SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(data, length), alpha_type);
    };
    return procs;
}

// A tiny solid-color PNG, the kind a real frame embeds via the image
// atlas. Encoded here so draw_image_from_data() has a decodable source.
sk_sp<SkData> solid_png(int size, SkColor color) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(size, size);
    sk_sp<SkSurface> raster = SkSurfaces::Raster(info);
    REQUIRE(raster != nullptr);
    raster->getCanvas()->clear(color);
    sk_sp<SkImage> image = raster->makeImageSnapshot();
    REQUIRE(image != nullptr);
    return SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
}

}  // namespace

TEST_CASE("SkpFrameCapture records vector ops into a round-tripping .skp",
          "[render][skia][skp]") {
    pulp::render::SkpFrameCapture capture(64, 48);
    REQUIRE(capture.available());
    REQUIRE(pulp::render::skp_capture_supported());

    pulp::canvas::Canvas* c = capture.canvas();
    REQUIRE(c != nullptr);
    c->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
    c->fill_rect(4.0f, 4.0f, 40.0f, 24.0f);
    c->set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255));
    c->fill_rect(20.0f, 20.0f, 24.0f, 16.0f);

    std::string blob;
    auto result = capture.finish_to_memory(blob);
    REQUIRE(result.ok);
    REQUIRE(result.bytes_written == blob.size());
    REQUIRE(result.op_count > 0);

    // The blob carries the documented .skp "skiapict" signature.
    REQUIRE(blob.size() >= 8);
    REQUIRE(std::memcmp(blob.data(), "skiapict", 8) == 0);

    // Deserialize and assert the recording survived.
    sk_sp<SkPicture> restored =
        SkPicture::MakeFromData(blob.data(), blob.size());
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == SkRect::MakeWH(64.0f, 48.0f));
    REQUIRE(restored->approximateOpCount() > 0);

    // The capture is consumed: canvas() is null, a second finish fails.
    REQUIRE(capture.canvas() == nullptr);
    REQUIRE_FALSE(capture.available());
    std::string second;
    REQUIRE_FALSE(capture.finish_to_memory(second).ok);
}

TEST_CASE("SkpFrameCapture writes a loadable .skp file", "[render][skia][skp]") {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "pulp-skp-capture-test.skp").string();
    std::filesystem::remove(path);

    auto result = pulp::render::capture_skp_to_file(
        32, 32, path, [](pulp::canvas::Canvas& c) {
            c.set_fill_color(pulp::canvas::Color::rgba8(0, 255, 0));
            c.fill_rect(0.0f, 0.0f, 32.0f, 32.0f);
        });
    REQUIRE(result.ok);
    REQUIRE(result.path == path);
    REQUIRE(result.bytes_written > 0);
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::file_size(path) == result.bytes_written);

    // Re-read the file off disk and confirm skiadebugger could load it.
    SkFILEStream in(path.c_str());
    REQUIRE(in.isValid());
    sk_sp<SkPicture> restored = SkPicture::MakeFromStream(&in);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == SkRect::MakeWH(32.0f, 32.0f));

    std::filesystem::remove(path);
}

// The env-var trigger is the concrete production caller the capture machinery
// was missing. Prove PULP_SKP_CAPTURE_DIR writes exactly one loadable .skp and
// then disarms (one-shot per env value).
TEST_CASE("maybe_capture_skp_from_env captures one .skp when PULP_SKP_CAPTURE_DIR is set",
          "[render][skia][skp]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pulp-skp-env-trigger-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto paint = [](pulp::canvas::Canvas& c) {
        c.set_fill_color(pulp::canvas::Color::rgba8(0, 128, 255));
        c.fill_rect(0.0f, 0.0f, 32.0f, 32.0f);
    };

    // Unset → honest no-op with a reason.
    pulp::test::unset_env_var("PULP_SKP_CAPTURE_DIR");
    auto none = pulp::render::maybe_capture_skp_from_env(32, 32, paint);
    REQUIRE_FALSE(none.ok);
    REQUIRE_FALSE(none.reason.empty());

    // Armed → captures one nonempty, loadable .skp into the directory.
    pulp::test::set_env_var("PULP_SKP_CAPTURE_DIR", dir.string().c_str());
    auto first = pulp::render::maybe_capture_skp_from_env(32, 32, paint);
    REQUIRE(first.ok);
    REQUIRE(fs::exists(first.path));
    REQUIRE(fs::file_size(first.path) > 0);
    SkFILEStream in(first.path.c_str());
    REQUIRE(in.isValid());
    REQUIRE(SkPicture::MakeFromStream(&in) != nullptr);

    // One-shot: a second call with the same env value does not re-capture.
    auto second = pulp::render::maybe_capture_skp_from_env(32, 32, paint);
    REQUIRE_FALSE(second.ok);

    // Exactly one .skp landed in the directory.
    std::size_t skp_count = 0;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".skp") ++skp_count;
    REQUIRE(skp_count == 1);

    pulp::test::unset_env_var("PULP_SKP_CAPTURE_DIR");
    fs::remove_all(dir, ec);
}

TEST_CASE("SkpFrameCapture preserves embedded-image pixels via fImageProc",
          "[render][skia][skp]") {
    // A frame that embeds an image must survive the .skp round trip with its
    // pixels intact.
    // SkpFrameCapture sets SkSerialProcs::fImageProc (PNG encode); if it
    // did not, the embedded image would deserialize to null and the
    // replay would be blank. Replay + pixel-sample is what proves the
    // payload survived — cullRect equality alone would still pass.
    sk_sp<SkData> png = solid_png(8, SK_ColorGREEN);
    REQUIRE(png != nullptr);

    pulp::render::SkpFrameCapture capture(8, 8);
    REQUIRE(capture.available());
    REQUIRE(capture.canvas()->draw_image_from_data(
        static_cast<const uint8_t*>(png->data()), png->size(),
        0.0f, 0.0f, 8.0f, 8.0f));

    std::string blob;
    auto result = capture.finish_to_memory(blob);
    REQUIRE(result.ok);
    REQUIRE(result.op_count > 0);

    // Deserialize WITH the matching fImageProc and replay into a fresh
    // raster surface pre-cleared to black; sample the top-left pixel.
    SkDeserialProcs dprocs = skp_deserial_procs();
    sk_sp<SkPicture> restored =
        SkPicture::MakeFromData(blob.data(), blob.size(), &dprocs);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == SkRect::MakeWH(8.0f, 8.0f));

    SkImageInfo info = SkImageInfo::MakeN32Premul(8, 8);
    REQUIRE(replay_top_left(restored, info) == SK_ColorGREEN);
}

// pulp #3656 — end-to-end raster PROOF that fill-rule actually changes the
// painted pixels (not just that the rule is threaded through the recording
// canvas, which the SvgPathWidget unit tests already cover). A compound
// path with two same-winding subpaths (outer + inner square) fills its
// center under nonzero winding (a solid block) but punches a hole under
// even-odd — the exact behavior a stroked-ellipse annulus needs. We render
// the real SkiaCanvas, serialize, replay onto a black raster surface, and
// sample the center pixel + a ring pixel.
TEST_CASE("SkiaCanvas fill-rule punches an even-odd hole in compound paths",
          "[render][skia][issue-3656]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    const SkImageInfo info = SkImageInfo::MakeN32Premul(W, H);

    auto paint_annulus = [&](pulp::canvas::FillRule rule) -> SkColor4f {
        pulp::render::SkpFrameCapture capture(W, H);
        REQUIRE(capture.available());
        auto* canvas = capture.canvas();
        REQUIRE(canvas != nullptr);

        // Outer + inner square, BOTH wound clockwise (same direction) so the
        // ONLY thing that changes the center is the winding rule.
        canvas->begin_path();
        canvas->move_to(6, 6);   canvas->line_to(42, 6);
        canvas->line_to(42, 42); canvas->line_to(6, 42);
        canvas->close_path();
        canvas->move_to(18, 18); canvas->line_to(30, 18);
        canvas->line_to(30, 30); canvas->line_to(18, 30);
        canvas->close_path();
        canvas->set_fill_color(pulp::canvas::Color{1.0f, 0.0f, 0.0f, 1.0f});  // red
        canvas->fill_current_path(rule);

        std::string blob;
        auto result = capture.finish_to_memory(blob);
        REQUIRE(result.ok);
        sk_sp<SkPicture> pic =
            SkPicture::MakeFromData(blob.data(), blob.size());
        REQUIRE(pic != nullptr);
        // Center (24,24) is inside BOTH loops; ring (10,24) is between them.
        return SkColor4f::FromColor(replay_pixel_at(pic, info, 24, 24));
    };
    auto ring_sample = [&](pulp::canvas::FillRule rule) -> SkColor4f {
        pulp::render::SkpFrameCapture capture(W, H);
        REQUIRE(capture.available());
        auto* canvas = capture.canvas();
        canvas->begin_path();
        canvas->move_to(6, 6);   canvas->line_to(42, 6);
        canvas->line_to(42, 42); canvas->line_to(6, 42);
        canvas->close_path();
        canvas->move_to(18, 18); canvas->line_to(30, 18);
        canvas->line_to(30, 30); canvas->line_to(18, 30);
        canvas->close_path();
        canvas->set_fill_color(pulp::canvas::Color{1.0f, 0.0f, 0.0f, 1.0f});
        canvas->fill_current_path(rule);
        std::string blob;
        REQUIRE(capture.finish_to_memory(blob).ok);
        sk_sp<SkPicture> pic =
            SkPicture::MakeFromData(blob.data(), blob.size());
        REQUIRE(pic != nullptr);
        return SkColor4f::FromColor(replay_pixel_at(pic, info, 10, 24));
    };

    // nonzero: both loops same winding → center has winding ±2 → FILLED red.
    const SkColor4f center_nonzero = paint_annulus(pulp::canvas::FillRule::nonzero);
    REQUIRE(center_nonzero.fR > 0.5f);
    REQUIRE(center_nonzero.fG < 0.25f);

    // evenodd: center is inside 2 loops → even → HOLE → stays black background.
    const SkColor4f center_evenodd = paint_annulus(pulp::canvas::FillRule::evenodd);
    REQUIRE(center_evenodd.fR < 0.25f);
    REQUIRE(center_evenodd.fG < 0.25f);

    // The ring (between the two squares) is filled red under BOTH rules —
    // proving even-odd renders the ring, it doesn't just blank the shape.
    REQUIRE(ring_sample(pulp::canvas::FillRule::nonzero).fR  > 0.5f);
    REQUIRE(ring_sample(pulp::canvas::FillRule::evenodd).fR > 0.5f);
#endif
}

// ── SkiaCanvas::stroke_path / fill_path raster proofs ────────────────────────
//
// These render the real SkiaCanvas through the same SkpFrameCapture →
// serialize → replay-onto-a-black-raster-surface path the fill-rule test above
// uses, then sample rendered pixels. They exist to pin the point-array
// polyline / polygon overrides: without them SkiaCanvas inherited the
// base-class fallbacks — stroke_path degraded to N independent butt-capped
// stroke_line calls (no joins between segments) and fill_path was a silent
// no-op (a filled polygon never appeared).
#ifdef PULP_HAS_SKIA
namespace {
// Render `draw` into a WxH capture, serialize, and sample one pixel color.
template <typename DrawFn>
SkColor4f render_and_sample(int w, int h, DrawFn draw, int x, int y) {
    pulp::render::SkpFrameCapture capture(w, h);
    REQUIRE(capture.available());
    auto* canvas = capture.canvas();
    REQUIRE(canvas != nullptr);
    draw(*canvas);
    std::string blob;
    REQUIRE(capture.finish_to_memory(blob).ok);
    sk_sp<SkPicture> pic = SkPicture::MakeFromData(blob.data(), blob.size());
    REQUIRE(pic != nullptr);
    const SkImageInfo info = SkImageInfo::MakeN32Premul(w, h);
    return SkColor4f::FromColor(replay_pixel_at(pic, info, x, y));
}
}  // namespace
#endif

TEST_CASE("SkiaCanvas fill_path fills a closed polygon", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    // A large triangle: apex top-center, base along the bottom. Its centroid
    // is comfortably interior; the top corners of the frame are exterior.
    const pulp::canvas::Canvas::Point2D tri[] = {
        {24.0f, 6.0f}, {42.0f, 42.0f}, {6.0f, 42.0f}};

    auto draw = [&](pulp::canvas::Canvas& c) {
        c.set_fill_color(pulp::canvas::Color{0.0f, 1.0f, 0.0f, 1.0f});  // green
        c.fill_path(tri, 3);
    };

    // A pixel strictly inside the triangle is the green fill. This is the
    // assertion that fails against the old silent no-op fill_path — with the
    // fallback nothing is drawn and (30,30) stays black background.
    const SkColor4f inside = render_and_sample(W, H, draw, 24, 30);
    REQUIRE(inside.fG > 0.5f);
    REQUIRE(inside.fR < 0.25f);
    REQUIRE(inside.fB < 0.25f);

    // A pixel outside the triangle (top-left corner) stays background.
    const SkColor4f outside = render_and_sample(W, H, draw, 3, 3);
    REQUIRE(outside.fG < 0.25f);
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

// Raster PROOF that SvgPathWidget's gradient stroke reaches painted pixels —
// the widget-level unit tests only assert the recording-canvas command
// stream. A thick horizontal line stroked with `to right, red → blue` must
// sample red near its left end and blue near its right end, and must NOT be
// the loud green solid fallback. This is the render path a Figma knob rim's
// GRADIENT_LINEAR stroke takes (setSvgStrokeGradient → set_stroke_gradient →
// Canvas::set_stroke_gradient_linear → SkGradientShader).
TEST_CASE("SvgPathWidget gradient stroke rasters the gradient, not the solid",
          "[render][skia][svg-path][stroke-gradient]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    pulp::view::SvgPathWidget w;
    w.set_path("M 2 24 L 46 24");
    w.set_viewbox(48, 48);
    w.set_bounds({0, 0, 48, 48});
    w.clear_fill();
    w.set_stroke_color(pulp::canvas::Color{0.0f, 1.0f, 0.0f, 1.0f});  // loud fallback
    w.set_stroke_width(10.0f);
    w.set_stroke_gradient("linear-gradient(to right, red, blue)");

    auto draw = [&](pulp::canvas::Canvas& c) { w.paint(c); };

    const SkColor4f left = render_and_sample(W, H, draw, 5, 24);
    REQUIRE(left.fR > 0.5f);
    REQUIRE(left.fG < 0.25f);   // not the green solid fallback

    const SkColor4f right = render_and_sample(W, H, draw, 43, 24);
    REQUIRE(right.fB > 0.5f);
    REQUIRE(right.fG < 0.25f);

    // Off the stroke band, the background stays black.
    const SkColor4f off = render_and_sample(W, H, draw, 24, 4);
    REQUIRE(off.fR < 0.25f);
    REQUIRE(off.fB < 0.25f);
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

#ifdef PULP_HAS_SKIA
namespace {
// A ring as ONE point array: an outer circle followed by an inner circle,
// BOTH wound the same direction, starting at angle 0 (so the two bridge
// segments are collinear along +x and cancel). Under even-odd the center is
// inside two contours → even → HOLE (a ring). Under nonzero the windings add
// → the center is FILLED (a disc). The point array is the only thing
// fill_path() takes, so the winding rule is the ONLY way to ask for the ring.
std::vector<pulp::canvas::Canvas::Point2D> ring_points(float cx, float cy,
                                                       float r_outer,
                                                       float r_inner,
                                                       int segments = 64) {
    std::vector<pulp::canvas::Canvas::Point2D> pts;
    pts.reserve(static_cast<size_t>(segments) * 2);
    for (float r : {r_outer, r_inner}) {
        for (int i = 0; i < segments; ++i) {
            const float t = 2.0f * 3.14159265358979f *
                            (static_cast<float>(i) / static_cast<float>(segments));
            pts.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
        }
    }
    return pts;
}
}  // namespace
#endif

// fill_path had NO FillRule parameter, so a two-contour ring point array
// always rendered as a filled disc — there was no way to ask for the hole.
TEST_CASE("SkiaCanvas fill_path honors FillRule -- evenodd rings, nonzero discs",
          "[render][skia][canvas]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    const auto pts = ring_points(24.0f, 24.0f, 20.0f, 10.0f);

    auto draw = [&](pulp::canvas::FillRule rule) {
        return [&, rule](pulp::canvas::Canvas& c) {
            c.set_fill_color(pulp::canvas::Color{0.0f, 1.0f, 0.0f, 1.0f});  // green
            c.fill_path(pts.data(), pts.size(), rule);
        };
    };

    // Center of the hole (24,24). evenodd → transparent (background stays
    // black); nonzero → filled green.
    const SkColor4f center_eo = render_and_sample(W, H, draw(pulp::canvas::FillRule::evenodd), 24, 24);
    INFO("evenodd center g=" << center_eo.fG);
    REQUIRE(center_eo.fG < 0.25f);

    const SkColor4f center_nz = render_and_sample(W, H, draw(pulp::canvas::FillRule::nonzero), 24, 24);
    INFO("nonzero center g=" << center_nz.fG);
    REQUIRE(center_nz.fG > 0.5f);

    // The ring band itself (r = 15 above the center) is filled under BOTH
    // rules — even-odd punches the hole, it does not blank the shape.
    REQUIRE(render_and_sample(W, H, draw(pulp::canvas::FillRule::evenodd), 24, 9).fG > 0.5f);
    REQUIRE(render_and_sample(W, H, draw(pulp::canvas::FillRule::nonzero), 24, 9).fG > 0.5f);

    // Outside the outer circle stays background under both.
    REQUIRE(render_and_sample(W, H, draw(pulp::canvas::FillRule::evenodd), 2, 2).fG < 0.25f);
    REQUIRE(render_and_sample(W, H, draw(pulp::canvas::FillRule::nonzero), 2, 2).fG < 0.25f);
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

// The retained-`Path` overload reaches Skia through its own converter
// (`to_sk_path`), so it needs its own raster proof: the point-array overload
// passing says nothing about this one, and a fill rule that never reaches the
// SkPath renders a ring as a solid disc with no error anywhere.
TEST_CASE("SkiaCanvas fill_path(Path) honors FillRule on a two-contour ring",
          "[render][skia][canvas]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    const auto pts = ring_points(24.0f, 24.0f, 20.0f, 10.0f);

    // Two genuine contours: outer ring closed, then inner ring closed. Both are
    // wound the same direction, so the winding rule is the only thing that can
    // decide the center.
    pulp::canvas::Path ring;
    ring.move_to(pts[0].x, pts[0].y);
    for (size_t i = 1; i < 64; ++i) ring.line_to(pts[i].x, pts[i].y);
    ring.close();
    ring.move_to(pts[64].x, pts[64].y);
    for (size_t i = 65; i < pts.size(); ++i) ring.line_to(pts[i].x, pts[i].y);
    ring.close();

    auto draw = [&](pulp::canvas::FillRule rule) {
        return [&, rule](pulp::canvas::Canvas& c) {
            c.set_fill_color(pulp::canvas::Color{0.0f, 1.0f, 0.0f, 1.0f});  // green
            c.fill_path(ring, rule);
        };
    };

    const SkColor4f center_eo =
        render_and_sample(W, H, draw(pulp::canvas::FillRule::evenodd), 24, 24);
    INFO("evenodd center g=" << center_eo.fG);
    REQUIRE(center_eo.fG < 0.25f);

    const SkColor4f center_nz =
        render_and_sample(W, H, draw(pulp::canvas::FillRule::nonzero), 24, 24);
    INFO("nonzero center g=" << center_nz.fG);
    REQUIRE(center_nz.fG > 0.5f);

    // The band between the rings is painted under BOTH rules.
    REQUIRE(render_and_sample(W, H, draw(pulp::canvas::FillRule::evenodd), 24, 9).fG > 0.5f);
    REQUIRE(render_and_sample(W, H, draw(pulp::canvas::FillRule::nonzero), 24, 9).fG > 0.5f);
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

TEST_CASE("SkiaCanvas stroke_path strokes a polyline without filling it",
          "[render][skia]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    // Open right-angle path: horizontal leg then vertical leg.
    const pulp::canvas::Canvas::Point2D poly[] = {
        {8.0f, 24.0f}, {40.0f, 24.0f}, {40.0f, 8.0f}};

    auto draw = [&](pulp::canvas::Canvas& c) {
        c.set_stroke_color(pulp::canvas::Color{1.0f, 0.0f, 0.0f, 1.0f});  // red
        c.set_line_width(4.0f);
        c.stroke_path(poly, 3);
    };

    // A pixel on the horizontal leg is stroked red.
    const SkColor4f on_path = render_and_sample(W, H, draw, 20, 24);
    REQUIRE(on_path.fR > 0.5f);

    // The interior of the (open) elbow is NOT filled — stroke_path strokes, it
    // does not fill. Sample well inside the bend, away from both legs.
    const SkColor4f interior = render_and_sample(W, H, draw, 20, 12);
    REQUIRE(interior.fR < 0.25f);
    REQUIRE(interior.fG < 0.25f);
    REQUIRE(interior.fB < 0.25f);
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

TEST_CASE("SkiaCanvas stroke_path joins segments into one continuous path",
          "[render][skia]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 48, H = 48;
    // A right-angle corner stroked thick with a ROUND join and BUTT caps.
    // As one SkPath, the round join fills a disk of radius line_width/2 at the
    // outer (south-east) corner of the vertex. As N independent butt-capped
    // stroke_line calls (the base-class fallback), the two segments meet flush
    // and leave that outer corner empty — the beading/gap this override fixes.
    const pulp::canvas::Canvas::Point2D corner[] = {
        {10.0f, 30.0f}, {30.0f, 30.0f}, {30.0f, 10.0f}};

    auto draw = [&](pulp::canvas::Canvas& c) {
        c.set_stroke_color(pulp::canvas::Color{1.0f, 0.0f, 0.0f, 1.0f});  // red
        c.set_line_width(8.0f);
        c.set_line_cap(pulp::canvas::LineCap::butt);
        c.set_line_join(pulp::canvas::LineJoin::round);
        c.stroke_path(corner, 3);
    };

    // (32,32) is ~2.83px from the vertex (30,30) — inside the round-join disk
    // (radius 4) but outside the butt-capped union of the two legs. Filled red
    // only when the polyline is stroked as one joined path. This assertion
    // fails against the base-class fallback.
    const SkColor4f join_corner = render_and_sample(W, H, draw, 32, 32);
    REQUIRE(join_corner.fR > 0.5f);

    // Sanity: a point on the horizontal leg is stroked under either code path.
    const SkColor4f on_leg = render_and_sample(W, H, draw, 20, 30);
    REQUIRE(on_leg.fR > 0.5f);
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

TEST_CASE("SkiaCanvas stroke_path / fill_path degenerate input is safe",
          "[render][skia]") {
#ifdef PULP_HAS_SKIA
    constexpr int W = 16, H = 16;
    const pulp::canvas::Canvas::Point2D one[] = {{8.0f, 8.0f}};

    // count 0, count 1, and a null pointer must draw nothing and must not
    // crash — matching CgCanvas. Every sampled pixel stays black background.
    auto expect_blank = [&](auto&& draw) {
        const SkColor4f px = render_and_sample(W, H, draw, 8, 8);
        REQUIRE(px.fR < 0.05f);
        REQUIRE(px.fG < 0.05f);
        REQUIRE(px.fB < 0.05f);
    };

    expect_blank([&](pulp::canvas::Canvas& c) {
        c.set_stroke_color(pulp::canvas::Color{1.0f, 1.0f, 1.0f, 1.0f});
        c.set_line_width(4.0f);
        c.stroke_path(one, 0);
        c.stroke_path(one, 1);
        c.stroke_path(nullptr, 0);
        c.stroke_path(nullptr, 5);
    });
    expect_blank([&](pulp::canvas::Canvas& c) {
        c.set_fill_color(pulp::canvas::Color{1.0f, 1.0f, 1.0f, 1.0f});
        c.fill_path(one, 0);
        c.fill_path(one, 1);
        c.fill_path(one, 2);      // < 3 points: no enclosed area
        c.fill_path(nullptr, 0);
        c.fill_path(nullptr, 5);
    });
#else
    SKIP("Skia not compiled in (PULP_HAS_SKIA undefined) — no raster backend");
#endif
}

TEST_CASE("SkpFrameCapture round-trips a GPU-texture-backed embedded image",
          "[render][skia][skp]") {
    // SkPicture::serialize()'s image proc must not silently drop GPU-texture-
    // backed embedded images. SkpFrameCapture rasterizes them via the
    // Graphite Context threaded through the capture, so a frame that embeds
    // a GPU image survives the .skp round trip with pixels intact. Requires
    // a live GPU adapter — skips cleanly without one.
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP(kNoGpu);
    GpuSurface::Config gpu_config{};
    gpu_config.width = 16;
    gpu_config.height = 16;
    if (!gpu->initialize(gpu_config)) SKIP(kNoGpu);
    if (gpu->adapter_info().backend_type == "Null")
        SKIP("Dawn Null backend cannot produce raster output");

    auto skia = SkiaSurface::create(*gpu, {.width = 16, .height = 16});
    if (!skia || !skia->is_available()) SKIP(kNoSkiaSurface);

    skgpu::graphite::Context* ctx = skia->graphite_context();
    REQUIRE(ctx != nullptr);

    // Build a GPU-texture-backed image: render solid green into an
    // offscreen Graphite render target and snapshot it. A Graphite
    // surface snapshot is a texture-backed SkImage — exactly the
    // atlas/snapshot case the fix targets. The recording must be
    // inserted + submitted so the texture actually holds the pixels.
    std::unique_ptr<skgpu::graphite::Recorder> recorder = ctx->makeRecorder();
    REQUIRE(recorder != nullptr);
    SkImageInfo info = SkImageInfo::MakeN32Premul(8, 8);
    sk_sp<SkSurface> gpu_surface =
        SkSurfaces::RenderTarget(recorder.get(), info);
    if (!gpu_surface) SKIP("offscreen Graphite render target unavailable");
    gpu_surface->getCanvas()->clear(SK_ColorGREEN);
    sk_sp<SkImage> gpu_image = gpu_surface->makeImageSnapshot();
    REQUIRE(gpu_image != nullptr);
    REQUIRE(gpu_image->isTextureBacked());
    {
        std::unique_ptr<skgpu::graphite::Recording> rec = recorder->snap();
        skgpu::graphite::InsertRecordingInfo rec_info{};
        rec_info.fRecording = rec.get();
        REQUIRE(ctx->insertRecording(rec_info) ==
                skgpu::graphite::InsertStatus::kSuccess);
        ctx->submit(skgpu::graphite::SyncToCpu::kYes);
    }

    pulp::render::SkpFrameCapture capture(8, 8, ctx);
    REQUIRE(capture.available());
    // Draw the texture-backed image into the capture canvas.
    auto* skia_canvas =
        static_cast<pulp::canvas::SkiaCanvas*>(capture.canvas());
    REQUIRE(skia_canvas != nullptr);
    REQUIRE(skia_canvas->draw_skia_image(gpu_image, 0.0f, 0.0f, 8.0f, 8.0f));

    std::string blob;
    auto result = capture.finish_to_memory(blob);
    REQUIRE(result.ok);
    REQUIRE(result.op_count > 0);

    // Deserialize with the matching image proc and replay. If the texture
    // image had been dropped, the replay would be black, not green.
    SkDeserialProcs dprocs = skp_deserial_procs();
    sk_sp<SkPicture> restored =
        SkPicture::MakeFromData(blob.data(), blob.size(), &dprocs);
    REQUIRE(restored != nullptr);
    REQUIRE(replay_top_left(restored, info) == SK_ColorGREEN);
}

TEST_CASE("SkpFrameCapture writes atomically — a failed write leaves no file",
          "[render][skia][skp]") {
    // The .skp must never land half-written and must never clobber a
    // previously-valid capture. The write goes to a sibling <dest>.tmp
    // and is renamed onto the destination only on full success.

    SECTION("failed write does not create the destination file") {
        // A path inside a directory that does not exist: opening the
        // sibling temp file fails, so nothing is written.
        const std::string bad_dir =
            (std::filesystem::temp_directory_path() /
             "pulp-skp-no-such-dir-xyz").string();
        std::filesystem::remove_all(bad_dir);
        const std::string dest = bad_dir + "/frame.skp";

        auto result = pulp::render::capture_skp_to_file(
            16, 16, dest, [](pulp::canvas::Canvas& c) {
                c.set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
                c.fill_rect(0.0f, 0.0f, 16.0f, 16.0f);
            });
        REQUIRE_FALSE(result.ok);
        REQUIRE_FALSE(result.reason.empty());
        REQUIRE_FALSE(std::filesystem::exists(dest));
        REQUIRE_FALSE(std::filesystem::exists(dest + ".tmp"));
    }

    SECTION("a failed write leaves a prior valid capture intact") {
        const std::string path =
            (std::filesystem::temp_directory_path() /
             "pulp-skp-atomic-prior.skp").string();
        std::filesystem::remove(path);

        // First capture succeeds and writes a valid .skp.
        auto first = pulp::render::capture_skp_to_file(
            16, 16, path, [](pulp::canvas::Canvas& c) {
                c.set_fill_color(pulp::canvas::Color::rgba8(0, 255, 0));
                c.fill_rect(0.0f, 0.0f, 16.0f, 16.0f);
            });
        REQUIRE(first.ok);
        REQUIRE(std::filesystem::exists(path));
        const auto original_size = std::filesystem::file_size(path);
        REQUIRE(original_size > 0);

        // A second capture aimed at the same path but with a temp file
        // that cannot be created: pre-create the <dest>.tmp path as a
        // NON-EMPTY directory. SkFILEWStream cannot open a directory as a
        // file, and the non-empty directory also survives the unlink
        // attempt — so the write fails and the destination is untouched.
        const std::string tmp_as_dir = path + ".tmp";
        std::filesystem::remove_all(tmp_as_dir);
        std::filesystem::create_directory(tmp_as_dir);
        { std::ofstream guard(tmp_as_dir + "/keep"); guard << "x"; }

        auto second = pulp::render::capture_skp_to_file(
            16, 16, path, [](pulp::canvas::Canvas& c) {
                c.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255));
                c.fill_rect(0.0f, 0.0f, 16.0f, 16.0f);
            });
        REQUIRE_FALSE(second.ok);
        REQUIRE_FALSE(second.reason.empty());

        // The prior capture is untouched — same file, same bytes.
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(std::filesystem::file_size(path) == original_size);
        SkFILEStream in(path.c_str());
        REQUIRE(in.isValid());
        sk_sp<SkPicture> restored = SkPicture::MakeFromStream(&in);
        REQUIRE(restored != nullptr);

        std::filesystem::remove_all(tmp_as_dir);
        std::filesystem::remove(path);
    }
}

TEST_CASE("SkpFrameCapture degrades gracefully on invalid input",
          "[render][skia][skp]") {
    // Non-positive dimensions yield an unavailable capture — no canvas,
    // no file, a clear reason. Never a crash or a partial .skp.
    pulp::render::SkpFrameCapture bad(0, 32);
    REQUIRE_FALSE(bad.available());
    REQUIRE(bad.canvas() == nullptr);

    std::string blob;
    auto mem = bad.finish_to_memory(blob);
    REQUIRE_FALSE(mem.ok);
    REQUIRE_FALSE(mem.reason.empty());
    REQUIRE(blob.empty());

    auto file = pulp::render::capture_skp_to_file(
        -1, -1, "/tmp/should-not-be-written.skp",
        [](pulp::canvas::Canvas&) {});
    REQUIRE_FALSE(file.ok);
    REQUIRE_FALSE(file.reason.empty());

    // Empty output path is rejected without writing.
    pulp::render::SkpFrameCapture ok(16, 16);
    REQUIRE(ok.available());
    auto empty_path = ok.finish_to_file("");
    REQUIRE_FALSE(empty_path.ok);
    REQUIRE_FALSE(empty_path.reason.empty());
}

#else  // !PULP_HAS_SKIA

TEST_CASE("SkpFrameCapture is unavailable without Skia", "[render][skia][skp]") {
    // The Skia-absent build must still link and degrade gracefully:
    // unavailable capture, null canvas, failed result with a reason.
    REQUIRE_FALSE(pulp::render::skp_capture_supported());

    pulp::render::SkpFrameCapture capture(64, 48);
    REQUIRE_FALSE(capture.available());
    REQUIRE(capture.canvas() == nullptr);

    std::string blob;
    auto mem = capture.finish_to_memory(blob);
    REQUIRE_FALSE(mem.ok);
    REQUIRE_FALSE(mem.reason.empty());

    auto file = pulp::render::capture_skp_to_file(
        64, 48, "/tmp/unused.skp", [](pulp::canvas::Canvas&) {});
    REQUIRE_FALSE(file.ok);
    REQUIRE_FALSE(file.reason.empty());

    // The env-var trigger links and degrades gracefully in a non-Skia build.
    auto env = pulp::render::maybe_capture_skp_from_env(
        64, 48, [](pulp::canvas::Canvas&) {});
    REQUIRE_FALSE(env.ok);
    REQUIRE_FALSE(env.reason.empty());
}

#endif  // PULP_HAS_SKIA
