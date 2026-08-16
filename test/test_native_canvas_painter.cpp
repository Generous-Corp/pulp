#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/render/headless_surface.hpp>
#include <pulp/view/canvas_widget.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::canvas::RendererBackend;
using pulp::view::CanvasWidget;
using pulp::view::NativeCanvasBackendRequirement;
using pulp::view::NativeCanvasPaintContext;
using pulp::view::NativeCanvasPainter;

namespace {

class CallbackPainter final : public NativeCanvasPainter {
public:
    explicit CallbackPainter(
        std::function<void(const NativeCanvasPaintContext&)> callback)
        : callback_(std::move(callback)) {}

    void paint(const NativeCanvasPaintContext& context) override {
        callback_(context);
    }

private:
    std::function<void(const NativeCanvasPaintContext&)> callback_;
};

class UnknownCanvas final : public RecordingCanvas {
public:
    RendererBackend renderer_backend() const noexcept override {
        return RendererBackend::unknown;
    }
};

class RasterIdentityCanvas final : public RecordingCanvas {
public:
    RendererBackend renderer_backend() const noexcept override {
        return RendererBackend::skia_raster;
    }
};

}  // namespace

TEST_CASE("CanvasWidget native painter receives local frame metadata") {
    CanvasWidget widget;
    widget.set_bounds({17.0f, 23.0f, 320.0f, 180.0f});

    std::vector<std::uint64_t> frames;
    auto painter = std::make_shared<CallbackPainter>(
        [&](const NativeCanvasPaintContext& context) {
            frames.push_back(context.frame_id);
            CHECK(context.local_bounds.x == 0.0f);
            CHECK(context.local_bounds.y == 0.0f);
            CHECK(context.local_bounds.width == 320.0f);
            CHECK(context.local_bounds.height == 180.0f);
            CHECK(context.backing_scale == 1.0f);
            CHECK(context.backend == RendererBackend::recording);
            context.canvas.set_fill_color(pulp::canvas::Color::hex(0x33cc99));
            context.canvas.fill_rect(2.0f, 3.0f, 4.0f, 5.0f);
        });
    widget.set_native_painter(painter);

    RecordingCanvas canvas;
    widget.paint(canvas);
    widget.paint(canvas);

    REQUIRE(frames == std::vector<std::uint64_t>{0, 1});
    REQUIRE(widget.last_native_paint_succeeded());
    REQUIRE(widget.last_native_paint_backend() == RendererBackend::recording);
    REQUIRE(canvas.save_count() == 0);
    REQUIRE(canvas.count(DrawCommand::Type::clip_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 2);
}

TEST_CASE("CanvasWidget native painter replacement and reset have safe lifetime") {
    CanvasWidget widget;
    widget.set_bounds({0.0f, 0.0f, 64.0f, 32.0f});
    RecordingCanvas canvas;

    int first_calls = 0;
    int second_calls = 0;
    std::weak_ptr<NativeCanvasPainter> first_weak;
    auto second = std::make_shared<CallbackPainter>(
        [&](const NativeCanvasPaintContext&) { ++second_calls; });
    {
        auto first = std::make_shared<CallbackPainter>(
            [&](const NativeCanvasPaintContext& context) {
                ++first_calls;
                // Deliberately leave state unbalanced and replace ourselves.
                context.canvas.save();
                widget.set_native_painter(second);
            });
        first_weak = first;
        widget.set_native_painter(first);
    }

    widget.paint(canvas);
    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 0);
    REQUIRE(first_weak.expired());
    REQUIRE(canvas.save_count() == 0);

    widget.paint(canvas);
    REQUIRE(second_calls == 1);
    widget.reset_native_painter();
    REQUIRE(widget.content_mode() == CanvasWidget::ContentMode::recorded_commands);
    second.reset();
    widget.paint(canvas);
    REQUIRE(second_calls == 1);
}

TEST_CASE("CanvasWidget Graphite Dawn requirement fails closed") {
    CanvasWidget widget;
    widget.set_bounds({0.0f, 0.0f, 80.0f, 40.0f});
    int calls = 0;
    widget.set_native_painter(
        std::make_shared<CallbackPainter>(
            [&](const NativeCanvasPaintContext&) { ++calls; }),
        NativeCanvasBackendRequirement::skia_dawn);

    RecordingCanvas recording;
    widget.paint(recording);
    REQUIRE(calls == 0);
    REQUIRE_FALSE(widget.last_native_paint_succeeded());
    REQUIRE(widget.last_native_paint_backend() == RendererBackend::recording);
    REQUIRE(recording.save_count() == 0);

    widget.set_native_painter(
        std::make_shared<CallbackPainter>(
            [&](const NativeCanvasPaintContext&) { ++calls; }),
        NativeCanvasBackendRequirement::skia_dawn_allow_recording);
    widget.paint(recording);
    REQUIRE(calls == 1);
    REQUIRE(widget.last_native_paint_succeeded());

    UnknownCanvas unknown;
    widget.paint(unknown);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(widget.last_native_paint_succeeded());
    REQUIRE(widget.last_native_paint_backend() == RendererBackend::unknown);

    RasterIdentityCanvas raster;
    widget.paint(raster);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(widget.last_native_paint_succeeded());
    REQUIRE(widget.last_native_paint_backend() ==
            RendererBackend::skia_raster);
}

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
TEST_CASE("CanvasWidget proves strict native paint on a live Dawn surface",
          "[gpu][skia][native-canvas-painter]") {
    pulp::render::HeadlessSurface::Config config;
    config.width = 96;
    config.height = 48;
    config.clear_r = 0;
    config.clear_g = 0;
    config.clear_b = 0;
    config.clear_a = 255;

    std::string error;
    auto surface = pulp::render::HeadlessSurface::create(config, &error);
    if (!surface) {
        SUCCEED("Dawn surface unavailable: " + error);
        return;
    }

    CanvasWidget widget;
    widget.set_bounds(
        {0.0f, 0.0f, static_cast<float>(config.width),
         static_cast<float>(config.height)});
    int calls = 0;
    widget.set_native_painter(
        std::make_shared<CallbackPainter>(
            [&](const NativeCanvasPaintContext& context) {
                ++calls;
                CHECK(context.backend == RendererBackend::skia_dawn);
                CHECK(context.backing_scale == 1.0f);
                context.canvas.set_fill_color(
                    pulp::canvas::Color::rgba8(32, 210, 120, 255));
                context.canvas.fill_rect(8.0f, 8.0f, 80.0f, 32.0f);
            }),
        NativeCanvasBackendRequirement::skia_dawn);

    auto frame = surface->render_rgba(
        [&](pulp::canvas::Canvas& canvas) { widget.paint(canvas); });
    if (frame.empty()) {
        SUCCEED("Dawn readback unavailable: " + surface->last_error());
        return;
    }

    REQUIRE(calls == 1);
    REQUIRE(widget.last_native_paint_succeeded());
    REQUIRE(widget.last_native_paint_backend() == RendererBackend::skia_dawn);

    std::size_t colored_pixels = 0;
    for (std::size_t i = 0; i + 3 < frame.pixels.size(); i += 4) {
        if (frame.pixels[i + 1] > 100) ++colored_pixels;
    }
    REQUIRE(colored_pixels > 1000);
}
#endif
