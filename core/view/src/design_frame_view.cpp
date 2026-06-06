#include <pulp/view/design_frame_view.hpp>

#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace pulp::view {

namespace {

// Parse width="" / height="" (px) from an <svg> header.
void svg_intrinsic_size(const std::string& svg, float& w, float& h) {
    auto num = [&](const char* key) -> float {
        const auto p = svg.find(key);
        if (p == std::string::npos) return 0.0f;
        const auto q = svg.find('"', p);
        if (q == std::string::npos) return 0.0f;
        return std::strtof(svg.c_str() + q + 1, nullptr);
    };
    w = num("width=");
    h = num("height=");
}

// The design PANEL = the largest <rect> that is a big fraction of the frame but
// not the whole frame (the body the window shows edge-to-edge); everything
// outside it is the Figma drop-shadow margin. Thresholds are relative to the
// frame so they hold for any design size.
void detect_panel(const std::string& svg, float frame_w, float frame_h,
                  float& px, float& py, float& pw, float& ph) {
    px = py = pw = ph = 0.0f;
    const float frame_area = frame_w * frame_h;
    float best = 0.0f;
    for (size_t p = svg.find("<rect "); p != std::string::npos;
         p = svg.find("<rect ", p + 1)) {
        float x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(svg.c_str() + p, "<rect x=\"%f\" y=\"%f\" width=\"%f\" height=\"%f\"",
                        &x, &y, &w, &h) != 4)
            continue;
        const float area = w * h;
        if (frame_area > 0.0f) {
            const float frac = area / frame_area;
            if (frac < 0.15f || frac > 0.97f) continue;  // skip decorations + full-frame bg
        }
        if (area > best) { best = area; px = x; py = y; pw = w; ph = h; }
    }
}

// Wrap the <path> element whose `d` equals `needle_d` in an SVG
// rotate(angle, cx, cy), so SkSVGDOM rotates only that needle.
void wrap_needle_rotation(std::string& svg, const std::string& needle_d,
                          float angle_deg, float cx, float cy) {
    const auto dp = svg.find(needle_d);
    if (dp == std::string::npos) return;
    const auto start = svg.rfind("<path", dp);
    auto end = svg.find("/>", dp);
    if (start == std::string::npos || end == std::string::npos) return;
    end += 2;
    char open[128];
    std::snprintf(open, sizeof(open), "<g transform=\"rotate(%.3f %.3f %.3f)\">",
                  angle_deg, cx, cy);
    svg.insert(end, "</g>");      // close first so `start` stays valid
    svg.insert(start, open);
}

constexpr float kSweepDeg = 270.0f;  // value 0->-135, 0.5->0 (up), 1->+135

}  // namespace

DesignFrameView::DesignFrameView(std::string svg, std::vector<DesignFrameElement> elements,
                                 float panel_x, float panel_y, float panel_w, float panel_h)
    : svg_(std::move(svg)), elements_(std::move(elements)) {
    svg_intrinsic_size(svg_, svg_w_, svg_h_);
    if (panel_w > 0 && panel_h > 0) {
        panel_x_ = panel_x; panel_y_ = panel_y; panel_w_ = panel_w; panel_h_ = panel_h;
    } else {
        detect_panel(svg_, svg_w_, svg_h_, panel_x_, panel_y_, panel_w_, panel_h_);
    }
    if (panel_w_ <= 0 || panel_h_ <= 0) {  // fallback: the whole frame
        panel_x_ = 0; panel_y_ = 0; panel_w_ = svg_w_; panel_h_ = svg_h_;
    }
}

float DesignFrameView::element_value(int i) const {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return -1.0f;
    return elements_[i].value;
}

void DesignFrameView::set_element_value(int i, float v) {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return;
    elements_[i].value = std::clamp(v, 0.0f, 1.0f);
    request_repaint();
}

DesignFrameView::PanelTransform DesignFrameView::panel_transform(const Rect& b) const {
    PanelTransform t;
    if (b.width <= 0 || b.height <= 0 || panel_w_ <= 0 || panel_h_ <= 0) return t;
    // Uniform fit, preserving the panel's aspect, centered in the view. When the
    // host sized the window to the panel aspect (the recommended path) the slack
    // is zero and the panel fills the view exactly; otherwise it letterboxes.
    t.scale = std::min(b.width / panel_w_, b.height / panel_h_);
    t.ox = (b.width - panel_w_ * t.scale) * 0.5f;
    t.oy = (b.height - panel_h_ * t.scale) * 0.5f;
    return t;
}

void DesignFrameView::paint(canvas::Canvas& canvas) {
    if (svg_.empty() || panel_w_ <= 0 || panel_h_ <= 0) return;
    std::string s = svg_;
    for (const auto& e : elements_)
        if (e.kind == DesignFrameElement::Kind::knob && !e.needle_d.empty())
            wrap_needle_rotation(s, e.needle_d, (e.value - 0.5f) * kSweepDeg, e.cx, e.cy);
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0) return;
    // Map the panel's top-left (panel_x_, panel_y_) to (ox, oy) at `scale`; the
    // surrounding shadow margin falls outside the view and is clipped. Same
    // transform hit_element() inverts, so knobs are hit where they're drawn.
    canvas.draw_svg(s, t.ox - panel_x_ * t.scale, t.oy - panel_y_ * t.scale,
                    svg_w_ * t.scale, svg_h_ * t.scale);
}

int DesignFrameView::hit_element(Point pos) const {
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0) return -1;
    // Invert the paint transform: view px -> SVG coords.
    const float sx = panel_x_ + (pos.x - t.ox) / t.scale;
    const float sy = panel_y_ + (pos.y - t.oy) / t.scale;
    int best = -1; float bd = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const float dx = sx - elements_[i].cx, dy = sy - elements_[i].cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < elements_[i].hit_radius && d < bd) { bd = d; best = i; }
    }
    return best;
}

void DesignFrameView::on_mouse_down(Point pos) {
    drag_ = hit_element(pos);
    if (drag_ >= 0) { drag_start_y_ = pos.y; drag_start_value_ = elements_[drag_].value; }
}

void DesignFrameView::on_mouse_drag(Point pos) {
    if (drag_ < 0) return;
    // Convert the vertical drag from view pixels into panel (design) space via the
    // same scale, so the turn sensitivity feels identical at any window size.
    const float scale = panel_transform(local_bounds()).scale;
    const float dy_design = scale > 0.0f ? (drag_start_y_ - pos.y) / scale : 0.0f;
    elements_[drag_].value =
        std::clamp(drag_start_value_ + dy_design * 0.005f, 0.0f, 1.0f);
    request_repaint();
}

}  // namespace pulp::view
