#pragma once

// DSP-free SuperConvolver editor built from real Pulp widgets.
//
// This view links NO audio code. It is driven entirely by a parameter table
// handed in from the host at init (index / name / range / default / unit) and
// talks back through three callbacks the entry TU forwards to JS. That is what
// lets ONE compiled UI module serve both the WAM and the WebCLAP demo: the host
// side of the seam is @danielraffel/web-player's HostAdapter
// (getParameterInfo / setParameterValue / onParamsChanged), which is ABI-agnostic.
//
// Widget values are normalized 0..1 (Knob's contract); the parameter table's
// min/max are the only place the real-unit mapping lives.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::webui {

namespace cv = pulp::canvas;
namespace vw = pulp::view;

struct ParamSpec {
    int index = 0;               ///< Position in the host's parameter table.
    std::string name;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    std::string unit;
    /// An ON/OFF parameter (the plugin declares it ParamKind::Toggle, which reaches us as
    /// a stepped 0..1). It gets a SWITCH, not a knob.
    ///
    /// A knob for a two-state value is simply the wrong control: it has no detents, no
    /// on/off affordance, and it renders as a dial reading "0.00" whose meaning nobody can
    /// guess. Bypass shipped that way. The rule is: if it is on/off, it is a switch.
    bool is_toggle = false;
};

class SuperConvolverWebUi : public vw::View {
public:
    /// UI -> host. `value` is in real units (min_value..max_value).
    std::function<void(int, float)> on_param_change;
    std::function<void(int)> on_gesture_begin;
    std::function<void(int)> on_gesture_end;

    explicit SuperConvolverWebUi(std::vector<ParamSpec> params) {
        // WITHOUT THIS, ANIMATIONS DRAW EXACTLY ONE FRAME AND STOP.
        //
        // The view only repaints when something marks it dirty. Toggle::on_mouse_down flips
        // the state and ANIMATES the thumb to its new position — an animation needs a stream
        // of frames to advance through. It got one: the frame that the value label's text
        // change happened to trigger. So Bypass read "On" while its thumb sat in the OFF
        // position, which is the worst possible state for a switch — the control and the
        // label disagreeing about what the plugin is doing.
        set_continuous_repaint(true);
        vw::Theme theme;
        if (const auto* preset = vw::find_preset("ink-signal"))
            theme = vw::theme_from_preset(*preset, /*dark=*/true);
        set_theme(theme);

        const cv::Color bg = theme.color("bg.primary")
                                 .value_or(cv::Color::rgba8(18, 19, 30));
        const cv::Color text = theme.color("text.primary")
                                   .value_or(cv::Color::rgba8(214, 221, 245));
        const cv::Color text_dim = theme.color("text.secondary")
                                       .value_or(cv::Color::rgba8(150, 158, 188));
        set_background_color(bg);

        flex().direction = vw::FlexDirection::column;
        flex().align_items = vw::FlexAlign::stretch;
        flex().padding = 20;
        flex().gap = 16;

        auto title = std::make_unique<vw::Label>("SuperConvolver");
        title->set_font_size(20);
        title->set_font_weight(700);
        title->set_text_color(text);
        title->flex().preferred_height = 26;
        title_ = title.get();
        add_child(std::move(title));

        auto row = std::make_unique<vw::View>();
        row->flex().direction = vw::FlexDirection::row;
        row->flex().justify_content = vw::FlexJustify::start;
        row->flex().align_items = vw::FlexAlign::start;
        row->flex().gap = 16;
        // NOT flex_grow — the knob row must size to its CONTENT.
        //
        // With flex_grow = 1 the row absorbed every spare pixel of the canvas and shoved
        // the status line to the very bottom, leaving a large empty band between the knobs
        // and the one line of text that explains which engine you are hearing. The two
        // belong together: the status is ABOUT the controls above it.
        //
        // Do not "fix" that gap by shrinking the canvas instead — this view lays out
        // proportionally, so a shorter box squeezes the rows into each other and the status
        // line lands on top of the knob labels (measured at 8/2.75 and 8/2.4; both collide).
        // Size the row to content, and the spare space collects at the BOTTOM where the
        // canvas can simply be shorter.
        row->flex().flex_grow = 0;
        auto* row_ptr = row.get();
        add_child(std::move(row));

        // NO status line in the plugin.
        //
        // The page renders an Engine <select> directly beneath this canvas — it says which
        // engine is selected, in words, and it is the control that changes it. Restating
        // that inside the view tree was duplicate chrome, and it was ALSO the element that
        // sheared through the knob labels at narrow widths, because a label whose text
        // changes on a timer is a layout event on a timer. The live numbers live in DOM
        // slots on the page (fixed widths, tabular figures). Nothing here has to move.

        cells_.reserve(params.size());
        for (auto& spec : params) {
            auto cell = std::make_unique<vw::View>();
            cell->flex().direction = vw::FlexDirection::column;
            cell->flex().align_items = vw::FlexAlign::center;
            cell->flex().gap = 6;
            cell->flex().preferred_width = kCellWidth;
            cell->flex().preferred_height = kCellHeight;

            // ON/OFF -> a switch. Everything else -> a knob. (See ParamSpec::is_toggle.)
            vw::Knob* knob_ptr = nullptr;
            vw::Toggle* toggle_ptr = nullptr;
            std::unique_ptr<vw::View> control;
            if (spec.is_toggle) {
                auto sw = std::make_unique<vw::Toggle>();
                sw->flex().preferred_width = kKnobSize;
                sw->flex().preferred_height = kKnobSize;
                sw->set_on(spec.default_value >= 0.5f, false);
                toggle_ptr = sw.get();
                control = std::move(sw);
            } else {
                auto knob = std::make_unique<vw::Knob>();
                knob->flex().preferred_width = kKnobSize;
                knob->flex().preferred_height = kKnobSize;
                knob->set_show_label(false);
                knob->set_value(normalize(spec, spec.default_value));
                knob->set_default_value(normalize(spec, spec.default_value));
                knob_ptr = knob.get();
                control = std::move(knob);
            }

            auto name = std::make_unique<vw::Label>(spec.name);
            name->set_font_size(13);
            name->set_text_color(text);
            name->set_text_align(vw::LabelAlign::center);
            name->flex().preferred_width = kCellWidth;
            name->flex().preferred_height = kLabelHeight;
            auto* name_ptr = name.get();

            auto value = std::make_unique<vw::Label>(format_value(spec, spec.default_value));
            value->set_font_size(12);
            value->set_text_color(text_dim);
            value->set_text_align(vw::LabelAlign::center);
            value->flex().preferred_width = kCellWidth;
            value->flex().preferred_height = kLabelHeight;
            auto* value_ptr = value.get();

            cell->add_child(std::move(control));
            cell->add_child(std::move(name));
            cell->add_child(std::move(value));
            row_ptr->add_child(std::move(cell));

            const int slot = static_cast<int>(cells_.size());
            if (knob_ptr) {
                knob_ptr->on_change = [this, slot](float normalized) {
                    on_knob_change(slot, normalized);
                };
                knob_ptr->on_gesture_begin = [this, slot]() {
                    if (on_gesture_begin) on_gesture_begin(cells_[slot].spec.index);
                };
                knob_ptr->on_gesture_end = [this, slot]() {
                    if (on_gesture_end) on_gesture_end(cells_[slot].spec.index);
                };
            } else if (toggle_ptr) {
                // A switch is one gesture: press = the whole edit. Bracket it so the host
                // groups it as a single undo step, exactly like a knob drag.
                toggle_ptr->on_toggle = [this, slot, toggle_ptr](bool on) {
                    const auto& sp = cells_[slot].spec;
                    // SNAP THE THUMB. Toggle::on_mouse_down flips the state and ANIMATES the
                    // thumb toward its new position — and an animation only advances if
                    // frames keep coming. A UI-initiated write also gets no echo back from
                    // the plugin (a CLAP plugin does not re-emit a parameter it was handed),
                    // so nothing else ever reconciles it either. The result was a switch
                    // whose label read "On" while its thumb sat in the OFF position — the
                    // control and the label disagreeing about what the plugin is doing.
                    //
                    // set_on(v, /*animate=*/false) with the state already flipped takes the
                    // reconcile path: it drives the thumb straight to its target and repaints.
                    // No dependency on a frame stream.
                    toggle_ptr->set_on(on, false);
                    if (on_gesture_begin) on_gesture_begin(sp.index);
                    if (on_param_change) on_param_change(sp.index, on ? sp.max_value : sp.min_value);
                    if (on_gesture_end) on_gesture_end(sp.index);
                    if (cells_[slot].value_label)
                        cells_[slot].value_label->set_text(format_value(sp, on ? sp.max_value : sp.min_value));
                };
            }

            cells_.push_back(Cell{std::move(spec), knob_ptr, toggle_ptr, name_ptr, value_ptr});
        }
    }

    /// Host -> UI. Does not re-emit on_param_change (Knob::set_value fires only
    /// the repaint path; on_change is raised from the pointer handlers).
    void set_param(int index, float real_value) {
        Cell* cell = find(index);
        if (!cell) return;
        if (cell->knob) cell->knob->set_value(normalize(cell->spec, real_value));
        else if (cell->toggle) cell->toggle->set_on(real_value >= 0.5f, false);
        cell->value_label->set_text(format_value(cell->spec, real_value));
    }

    /// Host -> UI. Replaces the status line under the knobs.
    void set_status(const std::string& text) {
        if (status_) status_->set_text(text.empty() ? kDefaultStatus : text);
    }

    /// Bounds of the status Label in root coordinates.
    bool status_bounds(vw::Rect* out) const {
        if (!status_) return false;
        if (out) *out = absolute_bounds(*status_);
        return true;
    }

    /// Real-unit value currently shown for `index`; NaN when unknown.
    float param_value(int index) const {
        for (const auto& cell : cells_) {
            if (cell.spec.index == index) return denormalize(cell.spec, cell.knob->value());
        }
        return std::numeric_limits<float>::quiet_NaN();
    }

    /// Bounds of the knob for `index` in root coordinates — the JS fixture
    /// needs a point to synthesize a pointer gesture over.
    bool knob_bounds(int index, vw::Rect* out) const {
        for (const auto& cell : cells_) {
            if (cell.spec.index != index) continue;
            if (out) *out = absolute_bounds(*cell.knob);
            return true;
        }
        return false;
    }

    /// Bounds of the name Label for `index` in root coordinates — the text
    /// region the fixture asserts is not empty.
    bool label_bounds(int index, vw::Rect* out) const {
        for (const auto& cell : cells_) {
            if (cell.spec.index != index) continue;
            if (out) *out = absolute_bounds(*cell.name_label);
            return true;
        }
        return false;
    }

    size_t param_count() const { return cells_.size(); }

private:
    static constexpr float kCellWidth = 92;
    static constexpr float kCellHeight = 128;
    static constexpr float kKnobSize = 72;
    static constexpr float kLabelHeight = 16;
    static constexpr float kStatusHeight = 34;   // two wrapped lines at 12 px
    static constexpr const char* kDefaultStatus =
        "Engine: CPU";

    struct Cell {
        ParamSpec spec;
        vw::Knob* knob = nullptr;       ///< set for a continuous parameter
        vw::Toggle* toggle = nullptr;   ///< set for an ON/OFF parameter
        vw::Label* name_label = nullptr;
        vw::Label* value_label = nullptr;
    };

    static float normalize(const ParamSpec& s, float real) {
        const float span = s.max_value - s.min_value;
        if (span == 0.0f) return 0.0f;
        return std::clamp((real - s.min_value) / span, 0.0f, 1.0f);
    }

    static float denormalize(const ParamSpec& s, float normalized) {
        return s.min_value + std::clamp(normalized, 0.0f, 1.0f) * (s.max_value - s.min_value);
    }

    static std::string format_value(const ParamSpec& s, float real) {
        // A switch reads Off/On. "0.00" under a toggle is a number pretending to be a state.
        if (s.is_toggle) return real >= 0.5f ? "On" : "Off";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f%s%s", real,
                      s.unit.empty() ? "" : " ", s.unit.c_str());
        return std::string(buf);
    }

    static vw::Rect absolute_bounds(const vw::View& v) {
        vw::Rect r = v.bounds();
        for (const vw::View* p = v.parent(); p != nullptr; p = p->parent()) {
            r.x += p->bounds().x;
            r.y += p->bounds().y;
        }
        return r;
    }

    Cell* find(int index) {
        for (auto& cell : cells_) {
            if (cell.spec.index == index) return &cell;
        }
        return nullptr;
    }

    void on_knob_change(int slot, float normalized) {
        Cell& cell = cells_[static_cast<size_t>(slot)];
        const float real = denormalize(cell.spec, normalized);
        cell.value_label->set_text(format_value(cell.spec, real));
        if (on_param_change) on_param_change(cell.spec.index, real);
    }

    vw::Label* title_ = nullptr;
    vw::Label* status_ = nullptr;
    std::vector<Cell> cells_;
};

}  // namespace pulp::webui
