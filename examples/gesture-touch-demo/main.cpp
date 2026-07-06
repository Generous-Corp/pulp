// Headless touch-gesture proof for Pulp's recognizer arbiter.
//
//   pulp-gesture-touch-demo --screenshot /tmp/pulp-gesture-touch-demo.png
//
// The binary builds a small touch-first UI, drives representative input through
// the arbiter, then captures the resulting state to a PNG.

#include <pulp/view/gesture.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
namespace canvas = pulp::canvas;

namespace {

constexpr uint32_t kWidth = 900;
constexpr uint32_t kHeight = 560;

canvas::Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return canvas::Color::rgba8(r, g, b);
}

canvas::Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return canvas::Color::rgba8(r, g, b, a);
}

MouseEvent event_at(Point position, MousePhase phase, int pointer_id = 0,
                    PointerType type = PointerType::mouse) {
    MouseEvent event;
    event.position = position;
    event.window_position = position;
    event.phase = phase;
    event.pointer_id = pointer_id;
    event.pointer_type = type;
    event.button = MouseButton::left;
    event.is_down = phase != MousePhase::release;
    return event;
}

std::unique_ptr<Label> label(std::string text, Rect bounds,
                             float size = 14.0f,
                             canvas::Color color = rgb(221, 231, 238),
                             int weight = 500) {
    auto out = std::make_unique<Label>(std::move(text));
    out->set_bounds(bounds);
    out->set_font_size(size);
    out->set_font_weight(weight);
    out->set_text_color(color);
    return out;
}

class DemoRoot final : public View {
public:
    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        c.set_fill_color(rgb(18, 22, 28));
        c.fill_rect(0, 0, bounds().width, bounds().height);
        c.set_fill_color(rgb(27, 33, 42));
        c.fill_rounded_rect(22, 72, bounds().width - 44, bounds().height - 94, 14);
    }
};

class GesturePad final : public View {
public:
    GesturePad() {
        auto pan = std::make_unique<PanRecognizer>();
        pan->set_min_distance(1.0f);
        pan->on_began = [this](GestureRecognizer& recognizer) {
            drag_start_x_ = x_;
            drag_start_y_ = y_;
            apply_pan(static_cast<PanRecognizer&>(recognizer));
        };
        pan->on_changed = [this](GestureRecognizer& recognizer) {
            apply_pan(static_cast<PanRecognizer&>(recognizer));
        };
        pan->on_ended = pan->on_changed;
        add_gesture_recognizer(std::move(pan));
    }

    float x_value() const { return x_; }
    float y_value() const { return y_; }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = bounds();
        c.set_fill_color(rgb(31, 38, 48));
        c.fill_rounded_rect(0, 0, b.width, b.height, 12);
        c.set_stroke_color(rgb(76, 91, 112));
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f, b.height - 1.0f, 12);

        c.set_stroke_color(rgba(112, 135, 158, 92));
        for (int i = 1; i < 4; ++i) {
            const float x = b.width * static_cast<float>(i) / 4.0f;
            const float y = b.height * static_cast<float>(i) / 4.0f;
            c.stroke_line(x, 12, x, b.height - 12);
            c.stroke_line(12, y, b.width - 12, y);
        }

        const float dot_x = 16.0f + x_ * (b.width - 32.0f);
        const float dot_y = 16.0f + (1.0f - y_) * (b.height - 32.0f);
        c.set_fill_color(rgba(30, 184, 176, 56));
        c.fill_circle(dot_x, dot_y, 22.0f);
        c.set_fill_color(rgb(30, 184, 176));
        c.fill_circle(dot_x, dot_y, 8.0f);
    }

private:
    void apply_pan(const PanRecognizer& pan) {
        const auto t = pan.translation();
        const auto b = bounds();
        x_ = std::clamp(drag_start_x_ + t.x / std::max(1.0f, b.width), 0.0f, 1.0f);
        y_ = std::clamp(drag_start_y_ - t.y / std::max(1.0f, b.height), 0.0f, 1.0f);
        request_repaint();
    }

    float x_ = 0.35f;
    float y_ = 0.62f;
    float drag_start_x_ = x_;
    float drag_start_y_ = y_;
};

class WaveformSurface final : public View {
public:
    WaveformSurface() {
        samples_.resize(512);
        for (size_t i = 0; i < samples_.size(); ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(samples_.size());
            samples_[i] = std::sin(t * 48.0f) * (0.35f + 0.55f * std::sin(t * 3.14159f));
        }

        auto pinch = std::make_unique<PinchRecognizer>();
        pinch->set_min_scale_delta(0.01f);
        pinch->on_began = [this](GestureRecognizer& recognizer) {
            pinch_start_zoom_ = zoom_;
            apply_pinch(static_cast<PinchRecognizer&>(recognizer));
        };
        pinch->on_changed = [this](GestureRecognizer& recognizer) {
            apply_pinch(static_cast<PinchRecognizer&>(recognizer));
        };
        pinch->on_ended = pinch->on_changed;
        add_gesture_recognizer(std::move(pinch));
    }

    float zoom() const { return zoom_; }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = bounds();
        c.set_fill_color(rgb(28, 33, 42));
        c.fill_rounded_rect(0, 0, b.width, b.height, 12);
        c.set_stroke_color(rgb(64, 78, 98));
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f, b.height - 1.0f, 12);

        c.set_stroke_color(rgba(87, 102, 126, 96));
        c.set_line_width(1.0f);
        const float mid_y = b.height * 0.52f;
        c.stroke_line(18, mid_y, b.width - 18, mid_y);

        canvas::Canvas::WaveformStyle style;
        style.line_color = rgb(133, 196, 255);
        style.fill_color = rgba(47, 132, 204, 50);
        style.line_thickness = 1.3f + zoom_ * 0.35f;
        c.draw_waveform(samples_.data(), samples_.size(),
                        20, 42, b.width - 40, b.height - 72, style);

        c.set_fill_color(rgb(30, 184, 176));
        c.fill_rounded_rect(20, 16, 48.0f * zoom_, 6, 3);
    }

private:
    void apply_pinch(const PinchRecognizer& pinch) {
        zoom_ = std::clamp(pinch_start_zoom_ * pinch.scale(), 0.75f, 3.0f);
        request_repaint();
    }

    std::vector<float> samples_;
    float zoom_ = 1.0f;
    float pinch_start_zoom_ = 1.0f;
};

class ListRow final : public View {
public:
    explicit ListRow(std::string text) : text_(std::move(text)) {
        auto pan = std::make_unique<PanRecognizer>();
        pan->set_min_distance(3.0f);
        pan->on_began = [this](GestureRecognizer& recognizer) {
            start_offset_ = offset_x_;
            apply_pan(static_cast<PanRecognizer&>(recognizer));
        };
        pan->on_changed = [this](GestureRecognizer& recognizer) {
            apply_pan(static_cast<PanRecognizer&>(recognizer));
        };
        pan->on_ended = pan->on_changed;
        row_pan_ = &add_gesture_recognizer(std::move(pan));
    }

    GestureRecognizer& row_pan() { return *row_pan_; }
    float offset_x() const { return offset_x_; }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = bounds();
        c.set_fill_color(rgb(42, 50, 63));
        c.fill_rounded_rect(offset_x_, 0, b.width, b.height, 8);
        c.set_fill_color(rgb(30, 184, 176));
        c.fill_rounded_rect(offset_x_ + 12, 11, 46, 10, 5);
        c.set_stroke_color(rgba(190, 208, 220, 70));
        c.stroke_line(offset_x_ + 74, 15, offset_x_ + b.width - 20, 15);
        c.stroke_line(offset_x_ + 74, 25, offset_x_ + b.width - 64, 25);
    }

private:
    void apply_pan(const PanRecognizer& pan) {
        offset_x_ = std::clamp(start_offset_ + pan.translation().x, 0.0f, 120.0f);
        request_repaint();
    }

    std::string text_;
    GestureRecognizer* row_pan_ = nullptr;
    float offset_x_ = 0.0f;
    float start_offset_ = 0.0f;
};

class ListSurface final : public View {
public:
    void attach_scroll_pan(GestureRecognizer& row_pan) {
        auto pan = std::make_unique<PanRecognizer>();
        pan->set_min_distance(4.0f);
        pan->on_began = [this](GestureRecognizer& recognizer) {
            start_scroll_ = scroll_y_;
            apply_pan(static_cast<PanRecognizer&>(recognizer));
        };
        pan->on_changed = [this](GestureRecognizer& recognizer) {
            apply_pan(static_cast<PanRecognizer&>(recognizer));
        };
        pan->on_ended = pan->on_changed;
        auto& scroll = add_gesture_recognizer(std::move(pan));
        scroll.require_to_fail(row_pan);
    }

    float scroll_y() const { return scroll_y_; }

    void layout_children() override {}

    void paint(canvas::Canvas& c) override {
        const auto b = bounds();
        c.set_fill_color(rgb(29, 35, 45));
        c.fill_rounded_rect(0, 0, b.width, b.height, 12);
        c.set_stroke_color(rgb(64, 78, 98));
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f, b.height - 1.0f, 12);
        c.set_fill_color(rgba(133, 196, 255, 44));
        c.fill_rounded_rect(18, 18 + scroll_y_, b.width - 36, 22, 7);
        c.fill_rounded_rect(18, 104 + scroll_y_, b.width - 36, 18, 6);
    }

private:
    void apply_pan(const PanRecognizer& pan) {
        scroll_y_ = std::clamp(start_scroll_ + pan.translation().y, -34.0f, 34.0f);
        request_repaint();
    }

    float scroll_y_ = 0.0f;
    float start_scroll_ = 0.0f;
};

struct DemoHandles {
    GesturePad* pad = nullptr;
    WaveformSurface* waveform = nullptr;
    ListSurface* list = nullptr;
    ListRow* row = nullptr;
};

std::unique_ptr<View> build_demo(DemoHandles& handles) {
    auto root = std::make_unique<DemoRoot>();
    root->set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});

    root->add_child(label("Gesture recognizer touch demo", {36, 24, 420, 32}, 24.0f,
                          rgb(244, 249, 252), 700));
    root->add_child(label("Arbiter-first pan, pinch, and nested scroll contention",
                          {38, 54, 520, 24}, 13.0f, rgb(155, 171, 186), 500));

    root->add_child(label("XY pad pan", {52, 92, 180, 22}, 15.0f, rgb(221, 231, 238), 650));
    auto pad = std::make_unique<GesturePad>();
    handles.pad = pad.get();
    pad->set_bounds({52, 122, 230, 230});
    root->add_child(std::move(pad));

    root->add_child(label("Pinch zoom waveform", {326, 92, 240, 22}, 15.0f,
                          rgb(221, 231, 238), 650));
    auto waveform = std::make_unique<WaveformSurface>();
    handles.waveform = waveform.get();
    waveform->set_bounds({326, 122, 522, 230});
    root->add_child(std::move(waveform));

    root->add_child(label("Nested row pan inside scroll", {52, 382, 260, 22}, 15.0f,
                          rgb(221, 231, 238), 650));
    auto list = std::make_unique<ListSurface>();
    handles.list = list.get();
    list->set_bounds({52, 412, 796, 108});

    auto row = std::make_unique<ListRow>("Gesture row");
    handles.row = row.get();
    row->set_bounds({18, 48, 620, 38});
    list->attach_scroll_pan(row->row_pan());
    list->add_child(std::move(row));
    root->add_child(std::move(list));

    return root;
}

bool drive_demo(View& root, const DemoHandles& handles) {
    double t = 0.0;
    bool ok = true;
    auto send = [&](MouseEvent event, double advance = 0.05) {
        t += advance;
        ok = root.dispatch_gesture_pointer_event(event, t) && ok;
    };

    send(event_at({110, 260}, MousePhase::press));
    send(event_at({205, 192}, MousePhase::drag));
    send(event_at({205, 192}, MousePhase::release));

    send(event_at({440, 230}, MousePhase::press, 1, PointerType::touch));
    send(event_at({720, 230}, MousePhase::press, 2, PointerType::touch));
    send(event_at({394, 230}, MousePhase::drag, 1, PointerType::touch));
    send(event_at({760, 230}, MousePhase::drag, 2, PointerType::touch));
    send(event_at({394, 230}, MousePhase::release, 1, PointerType::touch));
    send(event_at({760, 230}, MousePhase::release, 2, PointerType::touch));

    send(event_at({96, 478}, MousePhase::press));
    send(event_at({240, 478}, MousePhase::drag));
    send(event_at({240, 478}, MousePhase::release));

    ok = ok && handles.pad && handles.pad->x_value() > 0.60f;
    ok = ok && handles.waveform && handles.waveform->zoom() > 1.20f;
    ok = ok && handles.row && handles.row->offset_x() > 80.0f;
    ok = ok && handles.list && std::abs(handles.list->scroll_y()) < 1.0f;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::string screenshot_path = "/tmp/pulp-gesture-touch-demo.png";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            screenshot_path = argv[++i];
        }
    }

    DemoHandles handles;
    auto root = build_demo(handles);
    const bool proof_ok = drive_demo(*root, handles);
    const bool render_ok = render_to_file(*root, kWidth, kHeight, screenshot_path, 2.0f,
                                          ScreenshotBackend::default_backend);

    std::printf("%s gesture proof\n", proof_ok ? "passed" : "FAILED");
    std::printf("%s %s (%ux%u)\n", render_ok ? "wrote" : "FAILED",
                screenshot_path.c_str(), kWidth, kHeight);
    return proof_ok && render_ok ? 0 : 1;
}
