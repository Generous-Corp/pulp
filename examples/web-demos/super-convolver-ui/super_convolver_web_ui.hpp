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
};

class SuperConvolverWebUi : public vw::View {
public:
    /// UI -> host. `value` is in real units (min_value..max_value).
    std::function<void(int, float)> on_param_change;
    std::function<void(int)> on_gesture_begin;
    std::function<void(int)> on_gesture_end;

    explicit SuperConvolverWebUi(std::vector<ParamSpec> params) {
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
        row->flex().flex_grow = 1;
        auto* row_ptr = row.get();
        add_child(std::move(row));

        // One status line under the knobs. It is the ONLY surface in this view
        // that is not a parameter: the host pushes a formatted engine/miss/
        // budget string into it (pulp_ui_set_gpu_status). It carries no speed
        // claim — the GPU path is a capability demonstration, not a faster one.
        auto status = std::make_unique<vw::Label>(kDefaultStatus);
        status->set_font_size(12);
        status->set_text_color(text_dim);
        status->set_text_align(vw::LabelAlign::left);
        // The GPU line is long by design (engine, backend, block counts, the
        // µs-per-block against the real-time budget). Soft-wrap it: a single-line
        // Label would clip the budget figure off the right edge at demo widths,
        // and a truncated honesty statement is not one.
        status->set_multi_line(true);
        status->set_line_clamp(2);
        status->flex().preferred_height = kStatusHeight;
        status_ = status.get();
        add_child(std::move(status));

        cells_.reserve(params.size());
        for (auto& spec : params) {
            auto cell = std::make_unique<vw::View>();
            cell->flex().direction = vw::FlexDirection::column;
            cell->flex().align_items = vw::FlexAlign::center;
            cell->flex().gap = 6;
            cell->flex().preferred_width = kCellWidth;
            cell->flex().preferred_height = kCellHeight;

            auto knob = std::make_unique<vw::Knob>();
            knob->flex().preferred_width = kKnobSize;
            knob->flex().preferred_height = kKnobSize;
            knob->set_show_label(false);
            knob->set_value(normalize(spec, spec.default_value));
            knob->set_default_value(normalize(spec, spec.default_value));
            auto* knob_ptr = knob.get();

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

            cell->add_child(std::move(knob));
            cell->add_child(std::move(name));
            cell->add_child(std::move(value));
            row_ptr->add_child(std::move(cell));

            const int slot = static_cast<int>(cells_.size());
            knob_ptr->on_change = [this, slot](float normalized) {
                on_knob_change(slot, normalized);
            };
            knob_ptr->on_gesture_begin = [this, slot]() {
                if (on_gesture_begin) on_gesture_begin(cells_[slot].spec.index);
            };
            knob_ptr->on_gesture_end = [this, slot]() {
                if (on_gesture_end) on_gesture_end(cells_[slot].spec.index);
            };

            cells_.push_back(Cell{std::move(spec), knob_ptr, name_ptr, value_ptr});
        }
    }

    /// Host -> UI. Does not re-emit on_param_change (Knob::set_value fires only
    /// the repaint path; on_change is raised from the pointer handlers).
    void set_param(int index, float real_value) {
        Cell* cell = find(index);
        if (!cell) return;
        cell->knob->set_value(normalize(cell->spec, real_value));
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
        "Engine: CPU — convolution on the CPU (PartitionedConvolver)";

    struct Cell {
        ParamSpec spec;
        vw::Knob* knob = nullptr;
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
