#pragma once
// Native macro-knob UI. Ports Dream Date FX's DDDTheme::drawRotarySlider
// (stroked macro style) to Pulp's Canvas, and drives the Macro parameter via
// pulp::state::ParameterEdit gestures (begin/set/finish) so the value sticks
// and a host records automation. Interaction model copied verbatim from
// examples/bendr/src/reference_ui.hpp: the mac plugin host delivers down/up via
// BOTH on_mouse_event AND the legacy on_mouse_down/up, but drag ONLY via
// on_mouse_drag — so all paths funnel through pointer_press/move/release, which
// dedup on `down_`.

#include "processor.hpp"
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>
#include <cmath>

namespace knobpg {

namespace cv = pulp::canvas;
namespace vw = pulp::view;

inline constexpr float kPi = 3.14159265358979323846f;

class KnobUi : public vw::View {
public:
    explicit KnobUi(pulp::state::StateStore& store) : store_(store), edit_(store) {
        set_continuous_repaint(true);   // echo host automation each frame
        set_requires_gpu_host(true);    // Dawn/Skia/Metal
    }

    void paint(cv::Canvas& canvas) override {
        const float W = local_bounds().width, H = local_bounds().height;

        // background (DDIF lavender)
        canvas.set_fill_color(cv::Color::rgba8(0xdd, 0xd2, 0xeb));
        canvas.fill_rect(0, 0, W, H);

        // ── macro knob (ported from DDDTheme::drawRotarySlider, stroked) ──
        const float cx = W * 0.5f, cy = H * 0.44f;
        const float d = std::min(W, H) * 0.62f;
        const float s = d / 41.0f;            // base 41px voice-knob diameter
        const float sw = 6.0f * s;
        const float r = (d - sw) / 2.0f;
        // in-flight value during a drag, else the host value (automation echo)
        const float value = edit_.display_value(kMacro, store_.get_value(kMacro));
        const float a = kPi * 1.25f + value * (kPi * 1.5f);   // 270deg sweep

        // cream stroked ring
        canvas.set_stroke_color(cv::Color::rgba8(0xe8, 0xe1, 0xd5));
        canvas.set_line_width(sw);
        canvas.stroke_circle(cx, cy, r);

        // purple triangle pointer (largeTriangle proportions). fill_path does
        // NOT honour the canvas CTM, so rotate the points in code (absolute
        // coords) rather than via translate/rotate — same approach as bendr.
        const float triW = 8.5f * s, triH = 7.25f * s, gap = 1.25f * s + 1.0f;
        const float tr = r - sw / 2.0f - triH / 2.0f - gap;
        const float tcx = cx + tr * std::sin(a), tcy = cy - tr * std::cos(a);
        const float ca = std::cos(a), sa = std::sin(a);
        auto rot = [&](float px, float py) -> cv::Canvas::Point2D {
            return {px * ca - py * sa + tcx, px * sa + py * ca + tcy};
        };
        const cv::Canvas::Point2D tri[3] = {
            rot(-triW / 2.0f, triH / 2.0f), rot(triW / 2.0f, triH / 2.0f), rot(0.0f, -triH / 2.0f)};
        canvas.set_fill_color(cv::Color::rgba8(0x69, 0x78, 0x96));
        canvas.fill_path(tri, 3);

        // label (roughly centred under the knob)
        const float fs = 14.0f;
        canvas.set_fill_color(cv::Color::rgba8(0x69, 0x78, 0x96));
        canvas.set_font("Inter", fs);
        canvas.fill_text("Macro", cx - 18.0f, cy + r + fs + 6.0f);

        request_repaint();
    }

    // ── pointer handling (see header note: funnel + dedup) ──
    void on_mouse_event(const vw::MouseEvent& e) override {
        switch (e.phase) {
            case vw::MousePhase::press:   pointer_press(e.position); break;
            case vw::MousePhase::drag:    pointer_move(e.position); break;
            case vw::MousePhase::release: pointer_release(); break;
            case vw::MousePhase::hover:   break;
            case vw::MousePhase::automatic:
                if (e.is_down) { if (down_) pointer_move(e.position); else pointer_press(e.position); }
                break;
        }
    }
    void on_mouse_down(vw::Point p) override { pointer_press(p); }
    void on_mouse_drag(vw::Point p) override { pointer_move(p); }
    void on_mouse_up(vw::Point) override { pointer_release(); }

private:
    void pointer_press(vw::Point p) {
        if (down_) return;              // dedup: down arrives via two paths
        down_ = true;
        start_y_ = p.y;
        start_val_ = store_.get_value(kMacro);
        edit_.begin(kMacro);
    }
    void pointer_move(vw::Point p) {
        if (!down_) return;
        const float dv = (start_y_ - p.y) / 200.0f;   // drag up = increase
        edit_.set(kMacro, std::clamp(start_val_ + dv, 0.0f, 1.0f));
    }
    void pointer_release() {
        if (!down_) return;
        down_ = false;
        edit_.finish();
    }

    pulp::state::StateStore& store_;
    pulp::state::ParameterEdit edit_;
    bool down_ = false;
    float start_y_ = 0.0f, start_val_ = 0.0f;
};

} // namespace knobpg
