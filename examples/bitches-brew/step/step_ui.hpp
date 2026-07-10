#pragma once

// Step LFO's editor: the pattern, drawn as bars you can drag.
//
// Eight knobs would be the cheap way to expose eight levels, and it would be
// unusable — a pattern is a shape, and a shape has to be seen at once. So the
// bars are the control: drag one to set its level, and the bar that is currently
// playing lights up. On a plug-in that makes no sound, that highlight is the only
// thing distinguishing a running pattern from a stopped one.

#include "step_processor.hpp"

#include <brew/ui/panel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

/// Eight bars from a shared zero line, bipolar. Dragging sets a level; the
/// playing step is tinted.
class StepBars : public view::View {
public:
    StepBars(state::StateStore& store, const StepProcessor& proc)
        : store_(store), proc_(proc), edit_(store) {}

    void paint(canvas::Canvas& c) override {
        const float w = local_bounds().width, h = local_bounds().height;
        const float s = scale();
        const float mid = h * 0.5f;

        c.set_fill_color(ui::palette::rail);
        c.fill_rounded_rect(0, 0, w, h, 6.0f);

        const int len = proc_.length();
        const int playing = proc_.display_step();
        const float slot = w / static_cast<float>(kMaxSequencerSteps);
        const float pad = 3.0f * s;

        for (int i = 0; i < kMaxSequencerSteps; ++i) {
            const float x = slot * static_cast<float>(i);

            // Steps past the pattern's length never play. Draw them recessed
            // rather than hiding them: their values are still in the preset, and
            // lengthening the pattern brings them straight back.
            const bool live = i < len;
            const float v = store_.get_value(StepProcessor::step_param(i));
            const float top = mid - v * (mid - pad);

            c.set_fill_color(live ? (i == playing ? ui::palette::accent
                                                  : ui::palette::accent_dim)
                                  : ui::palette::border);
            const float y0 = std::min(top, mid), y1 = std::max(top, mid);
            c.fill_rect(x + pad, y0, slot - 2.0f * pad, std::max(y1 - y0, 1.0f * s));
        }

        c.set_stroke_color(ui::palette::border);
        c.set_line_width(1.0f * s);
        c.stroke_line(0, mid, w, mid);

        request_repaint();
    }

    void on_mouse_down(view::Point p) override {
        dragging_ = step_under(p);
        if (dragging_ >= 0) {
            edit_.begin(StepProcessor::step_param(dragging_));
            write(p);
        }
    }

    void on_mouse_drag(view::Point p) override {
        // Keep writing the step the gesture *started* on. Re-picking under the
        // cursor would let a sloppy vertical drag rewrite its neighbours, and the
        // gesture already belongs to one parameter for undo grouping.
        if (dragging_ >= 0) write(p);
    }

    void on_mouse_up(view::Point) override {
        if (dragging_ >= 0) edit_.finish();
        dragging_ = -1;
    }

private:
    [[nodiscard]] int step_under(view::Point p) const noexcept {
        const float w = local_bounds().width;
        if (w <= 0.0f || p.x < 0.0f || p.x >= w) return -1;
        const int i = static_cast<int>(p.x / (w / static_cast<float>(kMaxSequencerSteps)));
        return std::clamp(i, 0, kMaxSequencerSteps - 1);
    }

    void write(view::Point p) {
        const float h = local_bounds().height;
        if (h <= 0.0f) return;
        const float v = std::clamp(1.0f - 2.0f * (p.y / h), -1.0f, 1.0f);
        edit_.set(StepProcessor::step_param(dragging_), v);
    }

    state::StateStore& store_;
    const StepProcessor& proc_;
    state::ParameterEdit edit_;
    int dragging_ = -1;
};

class StepUi : public ui::BrewPanel {
public:
    StepUi(state::StateStore& store, const StepProcessor& proc)
        : ui::BrewPanel("Step LFO", "an eight-step pattern, locked to the host"),
          store_(store) {
        auto beats = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4g bt", v);
            return std::string(buf);
        };
        auto number = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            return std::string(buf);
        };
        auto whole = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return std::string(buf);
        };

        auto bars = std::make_unique<StepBars>(store_, proc);
        bars->flex().preferred_height = 130.0f;
        bars->flex().align_self = view::FlexAlign::stretch;

        auto pct = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%%", v * 100.0f);
            return std::string(buf);
        };

        auto add = [&](view::View& row, state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::knob_size(*k);
            row.add_child(std::move(k));
        };

        auto top = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*top, StepProcessor::kRate, "Rate", beats);
        add(*top, StepProcessor::kLength, "Length", whole);
        // How much of each step sounds. At 100% the gate never falls.
        add(*top, StepProcessor::kGate, "Gate", pct);
        add(*top, StepProcessor::kGlide, "Glide", number);
        add(*top, StepProcessor::kRandom, "Random", number);

        auto bottom = ui::row(ui::kRowGap, ui::kKnobHeight);
        add(*bottom, StepProcessor::kSeed, "Seed", whole);
        add(*bottom, StepProcessor::kOutputScale, "Out", number);
        auto per_step = ui::param_toggle(store_, StepProcessor::kSpeedMode, "Per Step");
        auto inv = ui::param_toggle(store_, StepProcessor::kInvert, "Invert");
        ui::toggle_size(*per_step);
        ui::toggle_size(*inv);
        bottom->add_child(std::move(per_step));
        bottom->add_child(std::move(inv));

        add_child(std::move(bars));
        add_child(std::move(top));
        add_child(std::move(bottom));
        // Two lines: one caption this long is clipped at the panel's width.
        add_child(ui::caption_label(
            "Rate is the whole pattern; Per Step makes it one step."));
        add_child(ui::caption_label(
            "Random hashes the step index, so a bounce repeats."));
    }

private:
    state::StateStore& store_;
};

}  // namespace pulp::examples::brew
