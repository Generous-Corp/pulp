// The Vellum side of the slice's editor. Compiled against the installed Vellum
// SDK only: every `pulp::view` / `pulp::canvas` name below is Vellum's mirrored
// unit, resolved from Vellum's shared libraries.
//
// The installed SDK packages no foreign-window host for a mirrored view tree
// (`PluginViewHost::create` lives in the unpackaged view-tooling unit), so
// frames leave through `render_to_rgba`, the raster path Vellum documents for
// a foreign host, and the Pulp side presents them.

#include "knob_panel.hpp"

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <atomic>
#include <utility>

namespace slice_vellum_ui {

namespace {

namespace view = pulp::view;
namespace canvas = pulp::canvas;

std::atomic<int> g_live_panels{0};

class MeterView final : public view::View {
  public:
    void set_level(float level) {
        level_ = std::clamp(level, 0.0f, 1.0f);
        request_repaint();
    }
    void paint(canvas::Canvas& c) override {
        const auto b = bounds();
        c.set_fill_color(canvas::Color::rgba8(0x22, 0x26, 0x2e));
        c.fill_rect(0, 0, b.width, b.height);
        const float h = b.height * level_;
        c.set_fill_color(canvas::Color::rgba8(0x4c, 0xd9, 0x64));
        c.fill_rect(0, b.height - h, b.width, h);
    }

  private:
    float level_ = 0.0f;
};

class Panel final : public KnobPanel {
  public:
    Panel(std::uint32_t width, std::uint32_t height, float initial, PanelCallbacks callbacks)
        : callbacks_(std::move(callbacks)), width_(width), height_(height) {
        ++g_live_panels;
        auto knob = std::make_unique<view::Knob>();
        knob_ = knob.get();
        knob_->set_value(initial);
        knob_->set_label("Gain");
        knob_->on_gesture_begin = [this] {
            if (callbacks_.begin)
                callbacks_.begin();
        };
        knob_->on_change = [this](float v) {
            if (!suppress_ && callbacks_.set)
                callbacks_.set(v);
        };
        knob_->on_gesture_end = [this] {
            if (callbacks_.end)
                callbacks_.end();
        };
        root_.add_child(std::move(knob));
        auto meter = std::make_unique<MeterView>();
        meter_ = meter.get();
        root_.add_child(std::move(meter));
        layout();
    }

    ~Panel() override {
        --g_live_panels;
    }

    void set_size(std::uint32_t width, std::uint32_t height, double) override {
        width_ = width;
        height_ = height;
        layout();
    }
    std::uint32_t width() const override {
        return width_;
    }
    std::uint32_t height() const override {
        return height_;
    }

    void value_changed(float normalized) override {
        suppress_ = true;
        knob_->set_value(normalized);
        suppress_ = false;
    }
    void set_meter(float level) override {
        meter_->set_level(level);
    }
    void idle() override {}

    float knob_value() const override {
        return knob_->value();
    }

    void drag_knob(float dy) override {
        root_.layout_children();
        const auto b = knob_->bounds();
        const view::Point centre{b.x + b.width * 0.5f, b.y + b.height * 0.5f};
        root_.simulate_drag(centre, {centre.x, centre.y + dy}, 10);
    }

    bool render_rgba(std::vector<std::uint8_t>& rgba, std::uint32_t& pixel_width,
                     std::uint32_t& pixel_height) override {
        rgba = view::render_to_rgba(root_, width_, height_, 1.0f, &pixel_width, &pixel_height);
        return !rgba.empty();
    }

    std::vector<std::uint8_t> capture_png() override {
        return view::render_to_png(root_, width_, height_, 1.0f, view::ScreenshotBackend::skia);
    }

  private:
    // Geometry goes through the flex layout every render runs, so the hit
    // test and the paint agree on where the knob is.
    void layout() {
        const float w = static_cast<float>(width_);
        const float h = static_cast<float>(height_);
        root_.set_bounds({0, 0, w, h});
        auto& root = root_.flex();
        root.direction = view::FlexDirection::row;
        root.padding = 8;
        root.gap = 8;
        const float knob_size = std::min(w * 0.6f, h - 16.0f);
        knob_->flex().preferred_width = knob_size;
        knob_->flex().preferred_height = knob_size;
        meter_->flex().preferred_width = 16;
        meter_->flex().preferred_height = h - 16;
        root_.layout_children();
    }

    PanelCallbacks callbacks_;
    std::uint32_t width_;
    std::uint32_t height_;
    view::View root_;
    view::Knob* knob_ = nullptr;
    MeterView* meter_ = nullptr;
    bool suppress_ = false;
};

} // namespace

KnobPanel::~KnobPanel() = default;

std::unique_ptr<KnobPanel> KnobPanel::create(std::uint32_t width, std::uint32_t height,
                                             float initial_value, PanelCallbacks callbacks) {
    return std::make_unique<Panel>(width, height, initial_value, std::move(callbacks));
}

int live_panels() {
    return g_live_panels.load();
}

} // namespace slice_vellum_ui
