#include <pulp/view/design_frame_view.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>

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
    build_overlays();
}

void DesignFrameView::build_overlays() {
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const auto& e = elements_[i];
        std::unique_ptr<View> widget;
        if (e.kind == DesignFrameElement::Kind::text_field) {
            // Opaque TextEditor over the design's search box: it paints its own
            // rounded bg (covering the baked box so there's no double-render),
            // shows the placeholder until focused, and draws an accent focus ring
            // on tap — the requested tap-to-focus + highlight behavior.
            auto editor = std::make_unique<TextEditor>();
            editor->placeholder = e.placeholder;
            // Match the design's dark field; the focus ring + caret come for free.
            editor->set_background_color(canvas::Color::rgba8(0x2c, 0x2d, 0x2d, 0xff));
            widget = std::move(editor);
        } else if (e.kind == DesignFrameElement::Kind::dropdown) {
            // Opaque ComboBox over the design's dropdown: it paints its own box +
            // selected text + chevron and opens a popup on click. Options come
            // from the source (else just the shown value).
            auto combo = std::make_unique<ComboBox>();
            if (!e.options.empty()) {
                combo->set_items(e.options);
                combo->set_selected_silent(
                    std::clamp(e.selected_index, 0,
                               static_cast<int>(e.options.size()) - 1));
            }
            widget = std::move(combo);
        } else if (e.kind == DesignFrameElement::Kind::tab_group &&
                   !e.options.empty()) {
            // Opaque segmented control over the design's tab strip; clicking a tab
            // moves the selection highlight (real selection state).
            widget = std::make_unique<DesignTabGroup>(
                e.options, std::clamp(e.selected_index, 0,
                                      static_cast<int>(e.options.size()) - 1));
        }
        if (widget) {
            View* raw = widget.get();
            add_child(std::move(widget));
            overlays_.push_back({i, raw});
        }
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

View* DesignFrameView::overlay_widget(int i) const {
    for (const auto& o : overlays_)
        if (o.element_index == i) return o.widget;
    return nullptr;
}

void DesignFrameView::layout_children() {
    // Position each native overlay over its element's rect, mapped through the
    // SAME panel transform paint() uses, so the widget tracks the design exactly
    // as the window scales (Codex: layout_children is the hook hosts/screenshots
    // call before paint; set_bounds only fires on_resized).
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0) return;
    for (const auto& o : overlays_) {
        const auto& e = elements_[o.element_index];
        o.widget->set_bounds({t.ox + (e.x - panel_x_) * t.scale,
                              t.oy + (e.y - panel_y_) * t.scale,
                              e.w * t.scale, e.h * t.scale});
    }
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
        // Only knobs are hit here; overlay controls own their hits via their child
        // widget (View::hit_test reaches children before this parent fallback).
        if (elements_[i].kind != DesignFrameElement::Kind::knob) continue;
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

// ── DesignTabGroup ──────────────────────────────────────────────────────────

DesignTabGroup::DesignTabGroup(std::vector<std::string> labels, int selected)
    : labels_(std::move(labels)),
      selected_(labels_.empty() ? 0
                                : std::clamp(selected, 0,
                                             static_cast<int>(labels_.size()) - 1)) {}

void DesignTabGroup::paint(canvas::Canvas& canvas) {
    const auto b = local_bounds();
    if (labels_.empty() || b.width <= 0 || b.height <= 0) return;
    const int n = static_cast<int>(labels_.size());
    const float slot = b.width / static_cast<float>(n);
    // Opaque dark strip (covers the baked tabs so there's no double-render).
    canvas.set_fill_color(canvas::Color::rgba8(0x25, 0x26, 0x26, 0xff));
    canvas.fill_rounded_rect(0, 0, b.width, b.height, std::min(b.height * 0.3f, 6.0f));
    // Selection highlight.
    if (selected_ >= 0 && selected_ < n) {
        const float pad = 2.0f;
        canvas.set_fill_color(canvas::Color::rgba8(0x3c, 0x3d, 0x3d, 0xff));
        canvas.fill_rounded_rect(selected_ * slot + pad, pad,
                                 slot - 2 * pad, b.height - 2 * pad,
                                 std::min((b.height - 2 * pad) * 0.35f, 5.0f));
    }
    // Labels, centered per slot; selected is brighter.
    const float font = std::min(b.height * 0.5f, 12.0f);
    canvas.set_font("Inter", font);
    for (int i = 0; i < n; ++i) {
        canvas.set_fill_color(i == selected_
                                  ? canvas::Color::rgba8(0xff, 0xff, 0xff, 0xff)
                                  : canvas::Color::rgba8(0x9a, 0x9a, 0x9a, 0xff));
        const float tw = canvas.measure_text(labels_[i]);
        const float tx = i * slot + (slot - tw) * 0.5f;
        const float ty = b.height * 0.5f + font * 0.34f;  // ~vertical center (baseline)
        canvas.fill_text(labels_[i], tx, ty);
    }
}

void DesignTabGroup::on_mouse_down(Point pos) {
    const auto b = local_bounds();
    if (labels_.empty() || b.width <= 0) return;
    const int n = static_cast<int>(labels_.size());
    int idx = static_cast<int>(pos.x / (b.width / static_cast<float>(n)));
    idx = std::clamp(idx, 0, n - 1);
    if (idx != selected_) { selected_ = idx; request_repaint(); }
}

}  // namespace pulp::view
