#include <pulp/view/design_frame_view.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/host_param_surface.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::view {

// ── Custom-control factory registry ──────────────────────────────────────────
// UI-thread-only (see the header contract), so a plain function-local static map
// with no locking is correct: registration at startup and lookup at overlay
// build both run on the UI thread.
namespace {
std::unordered_map<std::string, DesignControlFactory>& design_control_registry() {
    static std::unordered_map<std::string, DesignControlFactory> registry;
    return registry;
}
}  // namespace

void register_design_control_factory(std::string factory_id,
                                     DesignControlFactory factory) {
    if (factory_id.empty() || !factory) return;
    design_control_registry()[std::move(factory_id)] = std::move(factory);
}

bool has_design_control_factory(const std::string& factory_id) {
    const auto& reg = design_control_registry();
    return reg.find(factory_id) != reg.end();
}

void clear_design_control_factories() { design_control_registry().clear(); }

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
    const auto start = svg.rfind('<', dp);  // element start, any tag (rect/line/path)
    auto end = svg.find("/>", dp);
    if (start == std::string::npos || end == std::string::npos) return;
    end += 2;
    char open[128];
    std::snprintf(open, sizeof(open), "<g transform=\"rotate(%.3f %.3f %.3f)\">",
                  angle_deg, cx, cy);
    svg.insert(end, "</g>");      // close first so `start` stays valid
    svg.insert(start, open);
}

// Wrap the element containing `marker` in an SVG translate(dx, dy) — the fader
// analog of wrap_needle_rotation (moves a thumb without disturbing the chrome).
void wrap_thumb_translation(std::string& svg, const std::string& marker,
                            float dx, float dy) {
    const auto dp = svg.find(marker);
    if (dp == std::string::npos) return;
    const auto start = svg.rfind('<', dp);
    auto end = svg.find("/>", dp);
    if (start == std::string::npos || end == std::string::npos) return;
    end += 2;
    char open[96];
    std::snprintf(open, sizeof(open), "<g transform=\"translate(%.3f %.3f)\">", dx, dy);
    svg.insert(end, "</g>");
    svg.insert(start, open);
}

constexpr float kSweepDeg = 270.0f;  // value 0->-135, 0.5->0 (up), 1->+135

}  // namespace

bool suppress_svg_rect(std::string& svg, float x, float y, float w, float h,
                       float tol) {
    // Read a numeric attribute by its space-delimited key (" x=\"") so we don't
    // mistake rx=/cx=/ry= for x=/y=. Returns NaN when absent.
    auto attr = [](const std::string& tag, const char* spaced_key) -> float {
        const auto p = tag.find(spaced_key);
        if (p == std::string::npos) return std::numeric_limits<float>::quiet_NaN();
        const auto vs = p + std::string(spaced_key).size();
        const auto ve = tag.find('"', vs);
        if (ve == std::string::npos) return std::numeric_limits<float>::quiet_NaN();
        return std::strtof(tag.substr(vs, ve - vs).c_str(), nullptr);
    };
    std::size_t pos = 0;
    while ((pos = svg.find("<rect", pos)) != std::string::npos) {
        const auto end = svg.find('>', pos);
        if (end == std::string::npos) break;
        const std::string tag = svg.substr(pos, end - pos + 1);
        const float rx = attr(tag, " x=\""), ry = attr(tag, " y=\"");
        const float rw = attr(tag, " width=\""), rh = attr(tag, " height=\"");
        if (!std::isnan(rx) && !std::isnan(ry) && !std::isnan(rw) && !std::isnan(rh) &&
            std::fabs(rx - x) <= tol && std::fabs(ry - y) <= tol &&
            std::fabs(rw - w) <= tol && std::fabs(rh - h) <= tol) {
            svg.erase(pos, end - pos + 1);
            return true;
        }
        pos = end + 1;
    }
    return false;
}

bool suppress_svg_glow_at(std::string& svg, float x, float y, float w, float h) {
    const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    const char* kOpen = "<g filter=\"url(#";
    std::size_t pos = 0;
    while ((pos = svg.find(kOpen, pos)) != std::string::npos) {
        const auto gt = svg.find('>', pos);
        if (gt == std::string::npos) break;
        // First drawn coordinate of the group: the "d=\"M x y" of its first path.
        // A glyph glow's first point sits on the digit, inside the tab cell.
        float fx = std::numeric_limits<float>::quiet_NaN(), fy = fx;
        const auto dp = svg.find("d=\"M", gt);
        if (dp != std::string::npos && dp < gt + 600) {
            const char* p = svg.c_str() + dp + 4;
            while (*p == ' ') ++p;
            char* endp = nullptr;
            fx = std::strtof(p, &endp);
            if (endp != p) {
                p = endp;
                while (*p == ' ' || *p == ',') ++p;
                fy = std::strtof(p, &endp);
                if (endp == p) fy = std::numeric_limits<float>::quiet_NaN();
            } else {
                fx = std::numeric_limits<float>::quiet_NaN();
            }
        }
        if (!std::isnan(fx) && !std::isnan(fy) &&
            fx >= x0 && fx <= x1 && fy >= y0 && fy <= y1) {
            // Erase the whole group, depth-matching nested <g>…</g>.
            std::size_t i = gt + 1;
            int depth = 1;
            while (i < svg.size() && depth > 0) {
                const auto ng = svg.find("<g", i);
                const auto cg = svg.find("</g>", i);
                if (cg == std::string::npos) break;
                if (ng != std::string::npos && ng < cg) { depth++; i = ng + 2; }
                else { depth--; i = cg + 4; }
            }
            if (depth == 0) {
                svg.erase(pos, i - pos);
                return true;
            }
        }
        pos = gt + 1;
    }
    return false;
}

bool suppress_svg_glyph_at(std::string& svg, float x, float y, float w, float h) {
    const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    std::size_t pos = 0;
    while ((pos = svg.find("<path", pos)) != std::string::npos) {
        const auto end = svg.find("/>", pos);
        if (end == std::string::npos) break;
        const auto dp = svg.find("d=\"M", pos);
        float fx = std::numeric_limits<float>::quiet_NaN(), fy = fx;
        if (dp != std::string::npos && dp < end) {
            const char* p = svg.c_str() + dp + 4;
            while (*p == ' ') ++p;
            char* endp = nullptr;
            fx = std::strtof(p, &endp);
            if (endp != p) {
                p = endp;
                while (*p == ' ' || *p == ',') ++p;
                fy = std::strtof(p, &endp);
                if (endp == p) fy = std::numeric_limits<float>::quiet_NaN();
            } else {
                fx = std::numeric_limits<float>::quiet_NaN();
            }
        }
        if (!std::isnan(fx) && !std::isnan(fy) &&
            fx >= x0 && fx <= x1 && fy >= y0 && fy <= y1) {
            svg.erase(pos, end - pos + 2);
            return true;
        }
        pos = end + 2;
    }
    return false;
}

DesignFrameView::DesignFrameView(std::string svg, std::vector<DesignFrameElement> elements,
                                 float panel_x, float panel_y, float panel_w, float panel_h) {
    frames_.push_back(build_frame(std::move(svg), std::move(elements),
                                  panel_x, panel_y, panel_w, panel_h));
    activate_frame(0);
}

DesignFrameView::Frame DesignFrameView::build_frame(
    std::string svg, std::vector<DesignFrameElement> elements,
    float panel_x, float panel_y, float panel_w, float panel_h) const {
    Frame f;
    f.elements = std::move(elements);
    svg_intrinsic_size(svg, f.svg_w, f.svg_h);
    if (panel_w > 0 && panel_h > 0) {
        f.panel_x = panel_x; f.panel_y = panel_y; f.panel_w = panel_w; f.panel_h = panel_h;
    } else {
        detect_panel(svg, f.svg_w, f.svg_h, f.panel_x, f.panel_y, f.panel_w, f.panel_h);
    }
    if (f.panel_w <= 0 || f.panel_h <= 0) {  // fallback: the whole frame
        f.panel_x = 0; f.panel_y = 0; f.panel_w = f.svg_w; f.panel_h = f.svg_h;
    }

    // Suppress each design's BAKED selected-tab highlight so the live
    // DesignTabGroup pill is the ONLY highlight ever shown — otherwise switching
    // tabs leaves the baked pill behind at the original slot (a double-pill). The
    // baked highlight is a filled <rect> occupying the originally-selected slot;
    // remove it by geometry computed from the tab_group element (general — no
    // per-design constant). The strip background behind it remains, so the slot
    // reads as unselected until the live pill lands there.
    for (const auto& e : f.elements) {
        if (e.kind != DesignFrameElement::Kind::tab_group || e.options.empty())
            continue;
        const int n = static_cast<int>(e.options.size());
        const float slot_w = e.w / static_cast<float>(n);
        const int sel = std::clamp(e.selected_index, 0, n - 1);
        suppress_svg_rect(svg, e.x + static_cast<float>(sel) * slot_w, e.y,
                          slot_w, e.h);
        // Drop the BAKED tab digits so the live DesignTabGroup is the sole,
        // consistent renderer of them. Otherwise the live labels paint over the
        // baked ones — two slightly different glyphs (the design font vs the
        // overlay font) → a faint "doubled" look — and the baked SELECTED digit
        // keeps its glow + bright colour stuck in place when the live pill moves.
        // Per slot, remove the baked glyph: the selected one is a glow filter
        // group, the rest are plain <path>s; try the group first, then the path.
        for (int slot = 0; slot < n; ++slot) {
            const float cx = e.x + static_cast<float>(slot) * slot_w;
            if (!suppress_svg_glow_at(svg, cx, e.y, slot_w, e.h))
                suppress_svg_glyph_at(svg, cx, e.y, slot_w, e.h);
        }
    }
    // Drop the baked glyphs behind each live value_label readout, so the live
    // text (set_element_text) is the sole renderer and the readout tracks state
    // instead of showing the design's frozen mockup value. A readout is several
    // glyphs ("C2", "98") — remove every glyph whose first drawn coord falls in
    // the rect (loop until none remain), bounded so a runaway can't spin.
    for (const auto& e : f.elements) {
        if (e.kind != DesignFrameElement::Kind::value_label) continue;
        for (int guard = 0; guard < 8; ++guard)
            if (!suppress_svg_glyph_at(svg, e.x, e.y, e.w, e.h)) break;
    }
    f.svg = std::move(svg);
    return f;
}

void DesignFrameView::activate_frame(int index) {
    if (index < 0 || index >= static_cast<int>(frames_.size())) return;
    // Tear down the outgoing frame's overlay child widgets before swapping in
    // the new element set (build_overlays indexes into elements_).
    for (const auto& ov : overlays_)
        if (ov.widget) remove_child(ov.widget);
    overlays_.clear();
    drag_ = -1;

    const Frame& f = frames_[index];
    svg_ = f.svg;
    elements_ = f.elements;
    svg_w_ = f.svg_w; svg_h_ = f.svg_h;
    panel_x_ = f.panel_x; panel_y_ = f.panel_y;
    panel_w_ = f.panel_w; panel_h_ = f.panel_h;
    active_frame_ = index;
    build_overlays();
    invalidate_layout();
    on_active_frame_changed();
}

int DesignFrameView::add_frame(std::string svg, std::vector<DesignFrameElement> elements,
                               float panel_x, float panel_y, float panel_w, float panel_h) {
    frames_.push_back(build_frame(std::move(svg), std::move(elements),
                                  panel_x, panel_y, panel_w, panel_h));
    return static_cast<int>(frames_.size()) - 1;
}

void DesignFrameView::set_active_frame(int index) {
    if (index == active_frame_ || index < 0 ||
        index >= static_cast<int>(frames_.size()))
        return;
    // Release any held momentary key in the outgoing frame so notes don't stick
    // across a swap (mirrors set_active_view_group's release-on-switch edge).
    if (drag_ >= 0 && drag_ < static_cast<int>(elements_.size()) &&
        elements_[drag_].kind == DesignFrameElement::Kind::momentary) {
        elements_[drag_].value = 0.0f;
        if (on_gesture_end) on_gesture_end(drag_);
        drag_ = -1;
    }
    activate_frame(index);
    request_repaint();
}

void DesignFrameView::set_on_unregistered_custom_control(
    std::function<void(const UnregisteredCustomControl&)> cb) {
    on_unregistered_custom_ = std::move(cb);
    // The constructor already built the initial frame's overlays before a caller
    // could attach this callback, so replay any unregistered controls seen in the
    // most recent build. A callback set right after construction still learns
    // about them; thereafter it fires live from build_overlays on each rebuild.
    if (on_unregistered_custom_)
        for (const auto& miss : unregistered_custom_) on_unregistered_custom_(miss);
}

void DesignFrameView::build_overlays() {
    // Rebuilt fresh each activation: the diagnostic reflects the active frame's
    // current element set, not an accumulation across frame swaps.
    unregistered_custom_.clear();
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const auto& e = elements_[i];
        std::unique_ptr<View> widget;
        if (e.kind == DesignFrameElement::Kind::text_field) {
            // TextEditor over the design's search box: it shows the placeholder
            // until focused and draws an accent focus ring on tap. Its rect is
            // inset past the leading magnifier icon (which stays baked/visible),
            // and it paints the design's OWN field color when supplied so the
            // inset edge blends seamlessly with the baked box.
            auto editor = std::make_unique<TextEditor>();
            editor->placeholder = e.placeholder;
            canvas::Color bg = canvas::Color::rgba8(0x2c, 0x2d, 0x2d, 0xff);
            if (e.bg_color.size() >= 7 && e.bg_color[0] == '#') {
                try {
                    const unsigned v = static_cast<unsigned>(
                        std::stoul(e.bg_color.substr(1, 6), nullptr, 16));
                    bg = canvas::Color::rgba8((v >> 16) & 0xff, (v >> 8) & 0xff,
                                              v & 0xff, 0xff);
                } catch (...) { /* keep the default on malformed hex */ }
            }
            editor->set_background_color(bg);
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
            combo->on_change = [this, i](int idx) { notify_choice(i, idx); };
            widget = std::move(combo);
        } else if (e.kind == DesignFrameElement::Kind::tab_group &&
                   !e.options.empty()) {
            // Opaque segmented control over the design's tab strip; clicking a tab
            // moves the selection highlight (real selection state).
            auto tabs = std::make_unique<DesignTabGroup>(
                e.options, std::clamp(e.selected_index, 0,
                                      static_cast<int>(e.options.size()) - 1));
            tabs->on_select = [this, i](int idx) { notify_choice(i, idx); };
            widget = std::move(tabs);
        } else if (e.kind == DesignFrameElement::Kind::stepper &&
                   !e.options.empty()) {
            // `< >` stepper over the design's header preset selector: the value
            // text slides as the chevrons step through the options in place.
            auto stepper = std::make_unique<DesignStepper>(
                e.options, std::clamp(e.selected_index, 0,
                                      static_cast<int>(e.options.size()) - 1));
            stepper->on_select = [this, i](int idx) { notify_choice(i, idx); };
            widget = std::move(stepper);
        } else if (e.kind == DesignFrameElement::Kind::custom) {
            // Registered native control. Build the overlay via the factory
            // looked up under factory_id. If none is registered the element stays
            // inert — the baked SVG underneath still renders, so a custom control
            // never blanks. UI-thread-only, matching the registry contract.
            auto& reg = design_control_registry();
            const auto it = reg.find(e.factory_id);
            if (it != reg.end() && it->second) {
                DesignControlContext ctx;
                ctx.x = e.x; ctx.y = e.y; ctx.w = e.w; ctx.h = e.h;
                ctx.factory_id = e.factory_id;
                ctx.props = e.custom_props;
                ctx.source_node_id = e.source_node_id;
                ctx.default_value = e.value;
                widget = it->second(ctx);
                // If the factory-built control joined the value conduit, wire it
                // into the frame's channel. Seed the element's value FIRST, then
                // install the callbacks — so the seed can never re-emit (a frame
                // swap would otherwise let a contract-violating control clobber the
                // host param with its default). Matches the choice widgets'
                // seed-then-wire order. A control that didn't opt in is still built
                // and rendered — it just has no value channel.
                if (auto* dc = dynamic_cast<DesignControl*>(widget.get())) {
                    dc->set_control_value(e.value);
                    dc->on_control_changed = [this, i](float v) {
                        emit_element_changed(i, v);
                    };
                    dc->on_gesture_begin = [this, i]() { emit_gesture_begin(i); };
                    dc->on_gesture_end = [this, i]() { emit_gesture_end(i); };
                }
            } else {
                // No factory registered (or a null one): the element stays inert
                // and the baked SVG still shows — rendering is unchanged. Record
                // the id (de-duplicated, first-seen order) and, if a diagnostic
                // callback is set, hand it the id + source node so a developer or
                // a --validate check learns which controls were unwired.
                UnregisteredCustomControl miss{e.factory_id, e.source_node_id};
                const bool seen = std::any_of(
                    unregistered_custom_.begin(), unregistered_custom_.end(),
                    [&](const UnregisteredCustomControl& u) {
                        return u.factory_id == miss.factory_id;
                    });
                if (!seen) unregistered_custom_.push_back(miss);
                if (on_unregistered_custom_) on_unregistered_custom_(miss);
            }
        }
        if (widget) {
            View* raw = widget.get();
            add_child(std::move(widget));
            overlays_.push_back({i, raw});
        }
    }
}

DesignFrameElement::Kind DesignFrameView::element_kind(int i) const {
    if (i < 0 || i >= static_cast<int>(elements_.size()))
        return DesignFrameElement::Kind::knob;
    return elements_[i].kind;
}

float DesignFrameView::choice_to_norm(int i, int selected) const {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return 0.0f;
    const int n = static_cast<int>(elements_[i].options.size());
    if (n <= 1) return 0.0f;
    return std::clamp(selected, 0, n - 1) / static_cast<float>(n - 1);
}

int DesignFrameView::norm_to_choice(int i, float v) const {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return 0;
    const int n = static_cast<int>(elements_[i].options.size());
    if (n <= 1) return 0;
    return std::clamp(static_cast<int>(std::clamp(v, 0.0f, 1.0f) * (n - 1) + 0.5f),
                      0, n - 1);
}

void DesignFrameView::notify_choice(int i, int selected) {
    if (i >= 0 && i < static_cast<int>(elements_.size()))
        elements_[i].selected_index = selected;
    emit_element_changed(i, choice_to_norm(i, selected));
}

// ── Runtime host-parameter surface wiring ────────────────────────────────────

void DesignFrameView::emit_element_changed(int i, float value) {
    HostParamSurface* hp = route_to_host_params_ ? host_params() : nullptr;
    if (hp && i >= 0 && i < static_cast<int>(elements_.size())) {
        const std::string& key = elements_[i].param_key;
        if (!key.empty() && hp->has_param(key)) hp->set_param(key, value);
    }
    if (on_element_changed) on_element_changed(i, value);
}

void DesignFrameView::emit_gesture_begin(int i) {
    HostParamSurface* hp = route_to_host_params_ ? host_params() : nullptr;
    if (hp && i >= 0 && i < static_cast<int>(elements_.size())) {
        const std::string& key = elements_[i].param_key;
        if (!key.empty() && hp->has_param(key)) hp->begin_gesture(key);
    }
    if (on_gesture_begin) on_gesture_begin(i);
}

void DesignFrameView::emit_gesture_end(int i) {
    HostParamSurface* hp = route_to_host_params_ ? host_params() : nullptr;
    if (hp && i >= 0 && i < static_cast<int>(elements_.size())) {
        const std::string& key = elements_[i].param_key;
        if (!key.empty() && hp->has_param(key)) hp->end_gesture(key);
    }
    if (on_gesture_end) on_gesture_end(i);
}

void DesignFrameView::sync_from_host_params() {
    HostParamSurface* surface = host_params();
    if (!surface) return;  // preview/screenshot: degrade to local state
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const auto& e = elements_[i];
        if (e.param_key.empty() || !surface->has_param(e.param_key)) continue;
        const double norm = surface->get_param(e.param_key);
        if (e.kind == DesignFrameElement::Kind::value_label) {
            // A readout tracks its param's host-formatted display text.
            set_element_text(i, surface->param_display_text(e.param_key, norm));
        } else {
            // Silent host->view push (no echo back to the surface).
            set_element_value(i, static_cast<float>(norm));
        }
    }
}

// ── Per-element hover + enabled state ────────────────────────────────────────

void DesignFrameView::set_hovered_element(int i) {
    if (i == hovered_element_) return;
    const int prev = hovered_element_;
    hovered_element_ = i;
    if (on_element_hover) {
        if (prev >= 0) on_element_hover(prev, false);
        if (i >= 0) on_element_hover(i, true);
    }
    request_repaint();
}

void DesignFrameView::on_hover_move(Point pos) {
    set_hovered_element(hit_element(pos));  // hit_element already skips disabled
}

void DesignFrameView::on_mouse_leave() {
    set_hovered_element(-1);
}

// ── SVG fragment handles ─────────────────────────────────────────────────────

void DesignFrameView::register_fragment(std::string id, std::string marker) {
    if (id.empty() || marker.empty()) return;
    fragments_[std::move(id)] = std::move(marker);
}

bool DesignFrameView::current_svg_draw_box(float& ox, float& oy,
                                           float& ow, float& oh) const {
    if (svg_.empty() || panel_w_ <= 0 || panel_h_ <= 0) return false;
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0) return false;
    ox = t.ox - panel_x_ * t.scale;
    oy = t.oy - panel_y_ * t.scale;
    ow = svg_w_ * t.scale;
    oh = svg_h_ * t.scale;
    return true;
}

bool DesignFrameView::draw_fragment_marker(canvas::Canvas& canvas,
                                           const std::string& marker,
                                           const FragmentTransform& xform,
                                           float opacity,
                                           const std::string& recolor_hex) const {
    if (marker.empty()) return false;
    const std::string fragment = extract_svg_fragment(svg_, marker);
    if (fragment.empty()) return false;
    const std::string doc =
        build_svg_fragment_document(svg_, fragment, xform, opacity, recolor_hex);
    if (doc.empty()) return false;
    float x = 0, y = 0, w = 0, h = 0;
    if (!current_svg_draw_box(x, y, w, h)) return false;
    return canvas.draw_svg(doc, x, y, w, h);
}

bool DesignFrameView::draw_fragment(canvas::Canvas& canvas, const std::string& id,
                                    const FragmentTransform& xform, float opacity,
                                    const std::string& recolor_hex) const {
    const auto it = fragments_.find(id);
    if (it == fragments_.end()) return false;
    return draw_fragment_marker(canvas, it->second, xform, opacity, recolor_hex);
}

void DesignFrameView::set_element_enabled(int i, bool enabled) {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return;
    if (elements_[i].enabled == enabled) return;
    elements_[i].enabled = enabled;
    if (!enabled && hovered_element_ == i) set_hovered_element(-1);
    request_repaint();
}

void DesignFrameView::set_element_param_key(int i, std::string key) {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return;
    if (elements_[i].param_key == key) return;
    // Release the old key's gesture ONLY if one is actually open on this element
    // (i.e. it is the element currently being dragged). Ending an idle param's
    // gesture emits an end_gesture with no matching begin_gesture. Gesture
    // brackets are a strict protocol on every host parameter surface — an
    // unbalanced end may assert, drop the automation write, or leave the host's
    // undo grouping open — so only close a gesture this view actually opened.
    if (drag_ == i) {
        if (HostParamSurface* hp = route_to_host_params_ ? host_params() : nullptr;
            hp && !elements_[i].param_key.empty() && hp->has_param(elements_[i].param_key)) {
            hp->end_gesture(elements_[i].param_key);
        }
    }
    elements_[i].param_key = std::move(key);
    // Notify an owning key->index registry (e.g. the embed facade) to rebuild.
    if (on_param_key_changed) on_param_key_changed(i, elements_[i].param_key);
}

Rect DesignFrameView::element_rect(int i) const {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return {0, 0, 0, 0};
    const auto& e = elements_[i];
    return {e.x, e.y, e.w, e.h};
}

float DesignFrameView::element_value(int i) const {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return -1.0f;
    const auto& e = elements_[i];
    switch (e.kind) {
        case DesignFrameElement::Kind::knob:
        case DesignFrameElement::Kind::fader:
        case DesignFrameElement::Kind::toggle:
        case DesignFrameElement::Kind::xy_pad:
            return e.value;  // xy_pad: the X axis (value_y via the element directly)
        case DesignFrameElement::Kind::dropdown:
        case DesignFrameElement::Kind::tab_group:
        case DesignFrameElement::Kind::stepper: {
            int sel = e.selected_index;  // live widget wins when present
            if (View* w = overlay_widget(i)) {
                if (auto* c = dynamic_cast<ComboBox*>(w)) sel = c->selected();
                else if (auto* t = dynamic_cast<DesignTabGroup*>(w)) sel = t->selected();
                else if (auto* s = dynamic_cast<DesignStepper*>(w)) sel = s->selected();
            }
            return choice_to_norm(i, sel);
        }
        case DesignFrameElement::Kind::text_field:
            return -1.0f;  // text is not a normalized value
        case DesignFrameElement::Kind::momentary:
            return e.value;  // pressed/lit flag (0 or 1)
        case DesignFrameElement::Kind::custom:
            // A custom control that joined the value conduit reports its value;
            // one that didn't has no normalized value (unchanged sentinel).
            if (View* w = overlay_widget(i))
                if (auto* dc = dynamic_cast<DesignControl*>(w))
                    return dc->control_value();
            return -1.0f;
        case DesignFrameElement::Kind::swap:
        case DesignFrameElement::Kind::action:
        case DesignFrameElement::Kind::value_label:
            return -1.0f;  // buttons / labels: no standard normalized value
    }
    return -1.0f;
}

void DesignFrameView::set_element_text(int i, std::string text) {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return;
    if (elements_[i].kind != DesignFrameElement::Kind::value_label) return;
    if (elements_[i].text == text) return;
    elements_[i].text = std::move(text);
    request_repaint();
}

void DesignFrameView::set_element_value(int i, float v) {
    if (i < 0 || i >= static_cast<int>(elements_.size())) return;
    auto& e = elements_[i];
    switch (e.kind) {
        case DesignFrameElement::Kind::knob:
        case DesignFrameElement::Kind::fader:
        case DesignFrameElement::Kind::toggle:
        case DesignFrameElement::Kind::xy_pad:
            e.value = std::clamp(v, 0.0f, 1.0f);  // xy_pad: sets X; value_y unchanged
            request_repaint();
            break;
        case DesignFrameElement::Kind::dropdown:
        case DesignFrameElement::Kind::tab_group:
        case DesignFrameElement::Kind::stepper: {
            const int idx = norm_to_choice(i, v);
            e.selected_index = idx;
            if (View* w = overlay_widget(i)) {  // silent: host->view push, no echo
                if (auto* c = dynamic_cast<ComboBox*>(w)) c->set_selected_silent(idx);
                else if (auto* t = dynamic_cast<DesignTabGroup*>(w)) t->set_selected_silent(idx);
                else if (auto* s = dynamic_cast<DesignStepper*>(w)) s->set_selected_silent(idx);
            }
            break;
        }
        case DesignFrameElement::Kind::text_field:
            return;  // not a normalized value
        case DesignFrameElement::Kind::momentary:
            // Light/clear the key via the native overlay; no on_element_changed.
            e.value = (v > 0.5f) ? 1.0f : 0.0f;
            break;
        case DesignFrameElement::Kind::custom:
            // Silent host->view push to a custom control that joined the value
            // conduit; a control that didn't is unchanged (the factory owns its
            // own state). No on_element_changed echo, matching the other kinds.
            if (View* w = overlay_widget(i))
                if (auto* dc = dynamic_cast<DesignControl*>(w))
                    dc->set_control_value(std::clamp(v, 0.0f, 1.0f));
            request_repaint();  // guarantee the push reflects even if the control
                                // doesn't self-invalidate (automation/preset recall)
            return;
        case DesignFrameElement::Kind::swap:
        case DesignFrameElement::Kind::action:
        case DesignFrameElement::Kind::value_label:
            return;  // buttons / labels (no normalized value)
    }
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
    // as the window scales (layout_children is the hook hosts/screenshots
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
    for (const auto& e : elements_) {
        if (e.kind == DesignFrameElement::Kind::knob && !e.needle_d.empty())
            wrap_needle_rotation(s, e.needle_d, (e.value - 0.5f) * kSweepDeg, e.cx, e.cy);
        else if (e.kind == DesignFrameElement::Kind::fader && !e.needle_d.empty()) {
            // Translate the thumb along the track. Orientation is the track shape:
            // a wider-than-tall rect is a horizontal slider (value 0 → left e.x,
            // 1 → right e.x+e.w); otherwise vertical (value 1 → top e.y, 0 →
            // bottom e.y+e.h). e.cx/e.cy is the thumb's baked center.
            if (e.w > e.h) {
                const float target_x = e.x + e.value * e.w;
                wrap_thumb_translation(s, e.needle_d, target_x - e.cx, 0.0f);
            } else {
                const float target_y = e.y + (1.0f - e.value) * e.h;
                wrap_thumb_translation(s, e.needle_d, 0.0f, target_y - e.cy);
            }
        }
        else if (e.kind == DesignFrameElement::Kind::xy_pad && !e.needle_d.empty()) {
            // 2D puck: translate the puck element to (value, value_y) within the
            // pad rect [x,y,w,h]. value 0→left/1→right, value_y 0→top/1→bottom.
            const float target_x = e.x + e.value * e.w;
            const float target_y = e.y + e.value_y * e.h;
            wrap_thumb_translation(s, e.needle_d, target_x - e.cx, target_y - e.cy);
        }
        else if (e.kind == DesignFrameElement::Kind::toggle && !e.needle_d.empty()) {
            // A toggle WITH a dot marker is a switch: the baked dot position
            // (e.cx) is the OFF state; flipping ON slides it to the mirror across
            // the pill center (e.x + e.w/2). Using the mirror — rather than a
            // fixed left/right end — preserves the design's rest state whichever
            // side "off" sits on. The tint loop below recolours the track when on.
            const float dx_on = 2.0f * e.x + e.w - 2.0f * e.cx;   // baked → mirror
            wrap_thumb_translation(s, e.needle_d, e.value * dx_on, 0.0f);
        }
    }
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0) return;
    // Map the panel's top-left (panel_x_, panel_y_) to (ox, oy) at `scale`; the
    // surrounding shadow margin falls outside the view and is clipped. Same
    // transform hit_element() inverts, so knobs are hit where they're drawn.
    canvas.draw_svg(s, t.ox - panel_x_ * t.scale, t.oy - panel_y_ * t.scale,
                    svg_w_ * t.scale, svg_h_ * t.scale);

    // Per-element hover / bypass restyle (the visual half, riding the
    // fragment-handle primitive). We only have a reliable, pixel-exact handle for
    // elements whose moving part is a tagged SVG path (needle_d: knob needle,
    // fader/xy_pad thumb, switch dot); for those, redraw JUST that fragment over
    // the frame — a bypassed element desaturates to a muted grey, the hovered one
    // brightens with a translucent white recolor. Elements without a fragment
    // handle keep their baked chrome (honest — no guessed overlay). draw_fragment
    // never mutates the live document, so the dragged-knob patch above is intact.
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const auto& e = elements_[i];
        if (e.needle_d.empty()) continue;
        if (!e.enabled) {
            draw_fragment_marker(canvas, e.needle_d, {}, 0.45f, "#6b7280");
        } else if (i == hovered_element_) {
            draw_fragment_marker(canvas, e.needle_d, {}, 0.18f, "#ffffff");
        }
    }

    // Toggle buttons tint their rect when on, translucent over the baked chrome
    // so the S/M/icon label shows through.
    for (const auto& e : elements_) {
        if (e.kind != DesignFrameElement::Kind::toggle || e.value < 0.5f) continue;
        // Active tint: the design's own colour when it provides one (keeps
        // faithful imports pixel-true), else the theme accent so a reskin
        // recolours unspecified toggles instead of a baked-in default.
        const auto accent = resolve_color("accent.primary", canvas::Color::rgba8(20, 184, 166));
        unsigned r = accent.r8(), g = accent.g8(), b = accent.b8();
        if (e.bg_color.size() == 7 && e.bg_color[0] == '#') {
            r = std::strtoul(e.bg_color.substr(1, 2).c_str(), nullptr, 16);
            g = std::strtoul(e.bg_color.substr(3, 2).c_str(), nullptr, 16);
            b = std::strtoul(e.bg_color.substr(5, 2).c_str(), nullptr, 16);
        }
        // r/g/b come from the design colour or the theme accent above; 0x9c is
        // the translucency over the baked chrome (not a hardcoded theme colour).
        canvas.set_fill_color(canvas::Color::rgba8(static_cast<uint8_t>(r),  // token-lint:allow
                              static_cast<uint8_t>(g), static_cast<uint8_t>(b), 0x9c));
        canvas.fill_rounded_rect(t.ox + (e.x - panel_x_) * t.scale,
                                 t.oy + (e.y - panel_y_) * t.scale,
                                 e.w * t.scale, e.h * t.scale, 2.0f * t.scale);
    }

    // Native overlay highlight for lit momentary keys (value>0.5): the accent
    // fill of the key's exact shape, drawn ON TOP of the baked SVG (we never
    // recolor SVG paths — that would fight every re-export). The shape MUST
    // match the faithful key: square top, rounded BOTTOM corners (the design's
    // keys curve at the bottom, ~4 SVG units), and the top notched out around
    // any black keys so a lit white key never swallows them. View-scoped.
    //
    // The FILL mirrors the design's own pressed-key paint (paint36 in the
    // export): a vertical accent-teal gradient fading from 26% opacity at the
    // key top to 100% at the bottom (the "light mint top, deep teal bottom"
    // wash). It's set per key over the key's own height and applies identically
    // to white and black keys, so a lit key looks exactly like the figma.
    const auto teal = resolve_color("accent.primary", canvas::Color::rgba8(22, 218, 194));
    const canvas::Color grad_cols[2] = {
        canvas::Color::rgba(teal.r, teal.g, teal.b, 0.26f),  // top (offset 0)
        canvas::Color::rgba(teal.r, teal.g, teal.b, 1.0f),   // bottom (offset 1)
    };
    const float grad_pos[2] = {0.0f, 1.0f};
    const bool group_scoped = active_view_group_ != -1;
    auto in_view = [&](const DesignFrameElement& e) {
        return e.view_group == -1 || !group_scoped || e.view_group == active_view_group_;
    };
    const float kKeyCornerSvg = 4.0f;  // design's bottom-corner radius, SVG units
    for (const auto& e : elements_) {
        if (e.kind != DesignFrameElement::Kind::momentary || e.value <= 0.5f) continue;
        if (!in_view(e)) continue;
        const float rx = t.ox + (e.x - panel_x_) * t.scale;
        const float ry = t.oy + (e.y - panel_y_) * t.scale;
        const float rw = e.w * t.scale, rh = e.h * t.scale;

        // Control buttons (note < 0: octave/velocity/< >/pitch-bend/modulation/
        // sustain) are fully-rounded buttons, NOT piano keys — give them a
        // rounded-rect press highlight on ALL corners. The key-shape path below
        // (square top, rounded bottom, top notches) is for the actual keys; using
        // it on a rounded button left a square top that read as "cut off".
        if (e.note < 0) {
            const float cr = std::min({6.0f * t.scale, rw * 0.5f, rh * 0.5f});
            canvas.set_fill_gradient_linear(rx, ry, rx, ry + rh, grad_cols, grad_pos, 2);
            canvas.fill_rounded_rect(rx, ry, rw, rh, cr);
            continue;
        }

        const float r = std::min({kKeyCornerSvg * t.scale, rw * 0.5f, rh * 0.5f});

        // Collect every smaller momentary rect (same view) that GENUINELY notches
        // this key's TOP: overlaps in x, starts at/above the top, AND extends down
        // into the key. The reach-into test matters because a second keyboard in
        // the same frame (the piano below the typing row) shares the view group and
        // overlaps in x but not in y — its keys must NOT be mistaken for notches
        // (that drew a tall bar across the inter-keyboard gap). Notch bottoms are
        // clamped to the key. View-space x-spans, sorted left→right.
        struct Notch { float x0, x1, bottom; };
        std::vector<Notch> notches;
        const float my_area = e.w * e.h;
        for (const auto& b : elements_) {
            if (b.kind != DesignFrameElement::Kind::momentary || &b == &e) continue;
            if (!in_view(b) || b.w * b.h >= my_area) continue;
            if (b.x + b.w <= e.x || b.x >= e.x + e.w) continue;  // no x-overlap
            if (b.y > e.y + 2.0f) continue;                       // not a top notch
            if (b.y + b.h <= e.y) continue;                       // must reach into the key
            const float nx0 = std::max(b.x, e.x), nx1 = std::min(b.x + b.w, e.x + e.w);
            const float nb = std::min(b.y + b.h, e.y + e.h);
            notches.push_back({t.ox + (nx0 - panel_x_) * t.scale,
                               t.ox + (nx1 - panel_x_) * t.scale,
                               t.oy + (nb - panel_y_) * t.scale});
        }
        std::sort(notches.begin(), notches.end(),
                  [](const Notch& a, const Notch& c) { return a.x0 < c.x0; });

        // Per-key vertical gradient over the key's own height (top→bottom).
        canvas.set_fill_gradient_linear(rx, ry, rx, ry + rh, grad_cols, grad_pos, 2);

        // Build the key outline: top-left → across the top (dipping down and back
        // up around each notch) → top-right → down → rounded bottom-right →
        // across the bottom → rounded bottom-left → close.
        canvas.begin_path();
        canvas.move_to(rx, ry);
        for (const auto& n : notches) {
            canvas.line_to(n.x0, ry);
            canvas.line_to(n.x0, n.bottom);
            canvas.line_to(n.x1, n.bottom);
            canvas.line_to(n.x1, ry);
        }
        canvas.line_to(rx + rw, ry);
        canvas.line_to(rx + rw, ry + rh - r);
        canvas.quad_to(rx + rw, ry + rh, rx + rw - r, ry + rh);
        canvas.line_to(rx + r, ry + rh);
        canvas.quad_to(rx, ry + rh, rx, ry + rh - r);
        canvas.close_path();
        canvas.fill_current_path();
    }
    canvas.clear_fill_gradient();

    // Live value_label readouts: the baked glyph at each rect was suppressed in
    // build_frame, so paint the current `text` there (right-aligned, matching the
    // design's right-aligned numeric readouts) at the panel scale.
    for (const auto& e : elements_) {
        if (e.kind != DesignFrameElement::Kind::value_label || e.text.empty()) continue;
        float font = e.h * t.scale * 0.82f;          // ~rect height, like the baked glyph
        canvas.set_font("Inter", font);
        canvas.set_fill_color(canvas::Color::rgba8(0xF3, 0xF6, 0xF9, 0xff));  // faithful readout ink  token-lint:allow
        const float rx = t.ox + (e.x - panel_x_) * t.scale;
        const float ry = t.oy + (e.y - panel_y_) * t.scale;
        const float rw = e.w * t.scale, rh = e.h * t.scale;
        float tw = canvas.measure_text(e.text);
        // Auto-shrink-to-fit: a value wider than its rect (a 3-char reading like
        // "−20" or "C-2" in a box budgeted for a 1–2 char baked glyph) would draw
        // OVER the neighbouring label or button. Scale the font down so it fits the
        // rect width — the right edge stays put, so the reading never jumps
        // position, only the rare over-wide value renders a touch smaller.
        if (tw > rw && tw > 0.0f) {
            font *= rw / tw;
            canvas.set_font("Inter", font);
            tw = canvas.measure_text(e.text);
        }
        // Right-align in the rect (left-align if requested); baseline ~78% down.
        canvas.fill_text(e.text, e.value_left_align ? rx : rx + rw - tw, ry + rh * 0.78f);
    }
}

int DesignFrameView::hit_element(Point pos) const {
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0) return -1;
    // Invert the paint transform: view px -> SVG coords.
    const float sx = panel_x_ + (pos.x - t.ox) / t.scale;
    const float sy = panel_y_ + (pos.y - t.oy) / t.scale;

    // Momentary keys first: among rects containing the point, the SMALLEST-AREA
    // wins (a narrow black key beats the white key it overlaps), order-independent
    // so a re-export reorder can't change which key is hit. View-scoped.
    // Swap-link + action command buttons are always active (not view-scoped) and
    // take precedence so a toggle/control never gets masked by a key region.
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const auto& e = elements_[i];
        if (!e.enabled) continue;  // disabled/bypassed: not hit-testable
        if (e.kind != DesignFrameElement::Kind::swap &&
            e.kind != DesignFrameElement::Kind::action &&
            e.kind != DesignFrameElement::Kind::fader &&
            e.kind != DesignFrameElement::Kind::xy_pad &&
            e.kind != DesignFrameElement::Kind::toggle) continue;
        if (sx >= e.x && sx < e.x + e.w && sy >= e.y && sy < e.y + e.h) return i;
    }

    int key = -1; float key_area = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        const auto& e = elements_[i];
        if (!e.enabled) continue;
        if (e.kind != DesignFrameElement::Kind::momentary) continue;
        if (e.view_group != -1 && active_view_group_ != -1 && e.view_group != active_view_group_)
            continue;
        if (sx >= e.x && sx < e.x + e.w && sy >= e.y && sy < e.y + e.h) {
            const float area = e.w * e.h;
            if (area < key_area) { key_area = area; key = i; }
        }
    }
    if (key >= 0) return key;

    // Knobs: nearest within hit_radius. (Overlay controls own their hits via their
    // child widget — View::hit_test reaches children before this parent fallback.)
    int best = -1; float bd = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        if (elements_[i].kind != DesignFrameElement::Kind::knob) continue;
        if (!elements_[i].enabled) continue;
        const float dx = sx - elements_[i].cx, dy = sy - elements_[i].cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < elements_[i].hit_radius && d < bd) { bd = d; best = i; }
    }
    return best;
}

void DesignFrameView::on_mouse_down(Point pos) {
    const int hit = hit_element(pos);
    if (hit >= 0 && elements_[hit].kind == DesignFrameElement::Kind::swap) {
        // Swap-link button: change the rendered frame; no drag, no note.
        set_active_frame(elements_[hit].target_frame);
        return;
    }
    if (hit >= 0 && elements_[hit].kind == DesignFrameElement::Kind::action) {
        // Command button: fire the action; no drag, no note, no frame change.
        if (route_actions_to_host_) {
            if (HostActionSurface* actions = host_actions())
                actions->send_host_action(elements_[hit].action, "{}");
        }
        if (on_action) on_action(elements_[hit].action);
        return;
    }
    if (hit >= 0 && elements_[hit].kind == DesignFrameElement::Kind::toggle) {
        auto& e = elements_[hit];
        if (e.flash) {
            // Press-flash command button: light on press, clear on release.
            e.value = 1.0f;
            drag_ = hit;                 // so on_mouse_up clears it
        } else {
            e.value = e.value >= 0.5f ? 0.0f : 1.0f;  // sticky flip
        }
        request_repaint();
        emit_element_changed(hit, e.value);
        return;
    }
    drag_ = hit;
    if (drag_ < 0) return;
    if (elements_[drag_].kind == DesignFrameElement::Kind::momentary) {
        elements_[drag_].value = 1.0f;            // light the key
        request_repaint();
        if (on_gesture_begin) on_gesture_begin(drag_);  // note-on
        return;
    }
    if (elements_[drag_].kind == DesignFrameElement::Kind::xy_pad) {
        // Jump the puck to the click; the drag then tracks it absolutely.
        const auto t = panel_transform(local_bounds());
        if (t.scale > 0.0f) {
            auto& e = elements_[drag_];
            const float sx = panel_x_ + (pos.x - t.ox) / t.scale;
            const float sy = panel_y_ + (pos.y - t.oy) / t.scale;
            e.value   = std::clamp((sx - e.x) / e.w, 0.0f, 1.0f);
            e.value_y = std::clamp((sy - e.y) / e.h, 0.0f, 1.0f);
            request_repaint();
            emit_element_changed(drag_, e.value);
        }
    }
    drag_start_x_ = pos.x;
    drag_start_y_ = pos.y;
    drag_start_value_ = elements_[drag_].value;
    emit_gesture_begin(drag_);  // bracket the undo step
}

void DesignFrameView::on_mouse_drag(Point pos) {
    if (drag_ < 0) return;
    if (elements_[drag_].kind == DesignFrameElement::Kind::toggle) return;  // flash button — no drag
    if (elements_[drag_].kind == DesignFrameElement::Kind::momentary) {
        const int hit = hit_element(pos);
        if (hit == drag_) return;                 // still on the held key
        // Glissando / drag-off: release the held key; press the new one if the
        // pointer moved onto another momentary key.
        elements_[drag_].value = 0.0f;
        if (on_gesture_end) on_gesture_end(drag_);  // note-off (old)
        drag_ = -1;
        if (hit >= 0 && elements_[hit].kind == DesignFrameElement::Kind::momentary) {
            drag_ = hit;
            elements_[hit].value = 1.0f;
            if (on_gesture_begin) on_gesture_begin(hit);  // note-on (new)
        }
        request_repaint();
        return;
    }
    if (elements_[drag_].kind == DesignFrameElement::Kind::xy_pad) {
        // 2D puck: track the cursor absolutely within the pad rect.
        const auto t = panel_transform(local_bounds());
        if (t.scale > 0.0f) {
            auto& e = elements_[drag_];
            const float sx = panel_x_ + (pos.x - t.ox) / t.scale;
            const float sy = panel_y_ + (pos.y - t.oy) / t.scale;
            e.value   = std::clamp((sx - e.x) / e.w, 0.0f, 1.0f);
            e.value_y = std::clamp((sy - e.y) / e.h, 0.0f, 1.0f);
            request_repaint();
            emit_element_changed(drag_, e.value);
        }
        return;
    }
    // Knob/fader vertical drag: convert from view px into panel (design) space
    // via the same scale, so sensitivity feels identical at any window size. A
    // fader tracks the cursor 1:1 over its travel; a knob uses a fixed turn rate.
    const float scale = panel_transform(local_bounds()).scale;
    auto& el = elements_[drag_];
    // A horizontal fader (wider track than tall) tracks the cursor in X (right =
    // increase); a vertical fader / knob tracks Y (up = increase). Faders move
    // 1:1 over their travel; knobs use a fixed turn rate.
    const bool horizontal = el.kind == DesignFrameElement::Kind::fader && el.w > el.h;
    float delta_design, sens;
    if (horizontal) {
        delta_design = scale > 0.0f ? (pos.x - drag_start_x_) / scale : 0.0f;
        sens = el.w > 0.0f ? 1.0f / el.w : 0.005f;
    } else {
        delta_design = scale > 0.0f ? (drag_start_y_ - pos.y) / scale : 0.0f;
        sens = (el.kind == DesignFrameElement::Kind::fader && el.h > 0.0f)
                   ? 1.0f / el.h : 0.005f;
    }
    el.value = std::clamp(drag_start_value_ + delta_design * sens, 0.0f, 1.0f);
    request_repaint();
    // User-driven turn -> notify the binder (knob is value-bearing).
    emit_element_changed(drag_, elements_[drag_].value);
}

void DesignFrameView::on_mouse_up(Point /*pos*/) {
    if (drag_ >= 0) {
        if (elements_[drag_].kind == DesignFrameElement::Kind::momentary) {
            elements_[drag_].value = 0.0f;        // clear the key
            request_repaint();
            // Note-off: a momentary key is a MIDI note, NOT a param — route the
            // raw callback only, never host_params (mirrors the note-on path in
            // on_mouse_down, which returns before emit_gesture_begin).
            if (on_gesture_end) on_gesture_end(drag_);
        } else {
            if (elements_[drag_].kind == DesignFrameElement::Kind::toggle
                && elements_[drag_].flash) {
                elements_[drag_].value = 0.0f;    // press-flash: clear on release
                request_repaint();
                emit_element_changed(drag_, 0.0f);
            }
            emit_gesture_end(drag_);  // end undo step (routes to host if bound)
        }
    }
    drag_ = -1;
}

void DesignFrameView::set_active_view_group(int group) {
    if (group == active_view_group_) return;
    // Release any held key in the outgoing view so notes don't stick on a mode
    // switch (the contract's release-on-mode-switch edge).
    if (drag_ >= 0 && elements_[drag_].kind == DesignFrameElement::Kind::momentary) {
        elements_[drag_].value = 0.0f;
        if (on_gesture_end) on_gesture_end(drag_);
        drag_ = -1;
    }
    active_view_group_ = group;
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
    // NO opaque full-width strip: the design's tab/radio groups are NOT a solid
    // box — they're digits with a small highlight pill on the selected one,
    // sitting over content (e.g. the envelope graph). Painting a strip occludes
    // that content (the "floating box" bug). We draw ONLY the moving highlight
    // pill (matching the design's selected-pill) + the labels, so the rest of the
    // design shows through and only the selected slot reads as highlighted.
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

void DesignTabGroup::set_selected_silent(int index) {
    if (labels_.empty()) return;
    const int idx = std::clamp(index, 0, static_cast<int>(labels_.size()) - 1);
    if (idx != selected_) { selected_ = idx; request_repaint(); }
}

void DesignTabGroup::on_mouse_down(Point pos) {
    const auto b = local_bounds();
    if (labels_.empty() || b.width <= 0) return;
    const int n = static_cast<int>(labels_.size());
    int idx = static_cast<int>(pos.x / (b.width / static_cast<float>(n)));
    idx = std::clamp(idx, 0, n - 1);
    if (idx != selected_) {
        selected_ = idx;
        request_repaint();
        if (on_select) on_select(idx);  // user tap
    }
}

// ── DesignStepper ─────────────────────────────────────────────────────────

namespace {
const std::string kEmptyOption;
}  // namespace

DesignStepper::DesignStepper(std::vector<std::string> options, int selected)
    : options_(std::move(options)),
      selected_(options_.empty()
                    ? 0
                    : std::clamp(selected, 0,
                                 static_cast<int>(options_.size()) - 1)) {}

const std::string& DesignStepper::current() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(options_.size()))
        return kEmptyOption;
    return options_[selected_];
}

void DesignStepper::paint(canvas::Canvas& canvas) {
    const auto b = local_bounds();
    if (options_.empty() || b.width <= 0 || b.height <= 0) return;
    // A single option has nowhere to step: there is no live value to show beyond
    // what the design already baked. Painting the chevrons + value here would
    // double them on top of the baked header (the "doubled header text" bug). Let
    // the baked SVG show through untouched; the overlay stays as a (no-op) hit
    // target so a future multi-option list lights it up without re-importing.
    if (options_.size() <= 1) return;
    const float font = std::min(b.height * 0.5f, 12.0f);
    canvas.set_font("Inter", font);
    const float ty = b.height * 0.5f + font * 0.34f;  // baseline ~vertical center
    // Chevrons hug the edges; dimmer when there's nowhere further to step.
    const bool can_prev = selected_ > 0;
    const bool can_next = selected_ < static_cast<int>(options_.size()) - 1;
    const auto dim = canvas::Color::rgba8(0x6a, 0x6a, 0x6a, 0xff);
    const auto lit = canvas::Color::rgba8(0xcf, 0xcf, 0xcf, 0xff);
    canvas.set_fill_color(can_prev ? lit : dim);
    canvas.fill_text("<", 2.0f, ty);
    canvas.set_fill_color(can_next ? lit : dim);
    canvas.fill_text(">", b.width - 2.0f - canvas.measure_text(">"), ty);
    // Current value, centered between the chevrons.
    canvas.set_fill_color(canvas::Color::rgba8(0xff, 0xff, 0xff, 0xff));
    const std::string& val = current();
    const float tw = canvas.measure_text(val);
    canvas.fill_text(val, (b.width - tw) * 0.5f, ty);
}

void DesignStepper::set_selected_silent(int index) {
    if (options_.empty()) return;
    const int idx = std::clamp(index, 0, static_cast<int>(options_.size()) - 1);
    if (idx != selected_) { selected_ = idx; request_repaint(); }
}

void DesignStepper::on_mouse_down(Point pos) {
    const auto b = local_bounds();
    if (options_.empty() || b.width <= 0) return;
    const int n = static_cast<int>(options_.size());
    int next = selected_;
    if (pos.x < b.width * 0.5f) next = selected_ - 1;  // left half: previous
    else                        next = selected_ + 1;  // right half: next
    next = std::clamp(next, 0, n - 1);
    if (next != selected_) {
        selected_ = next;
        request_repaint();
        if (on_select) on_select(next);  // user step
    }
}

}  // namespace pulp::view
