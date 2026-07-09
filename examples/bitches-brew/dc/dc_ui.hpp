#pragma once

// DC's editor: one value, and proof of where it went.
//
// The readout is the point. A CV plug-in produces no sound and draws no meter in
// the host, so without a rail the user cannot distinguish "holding +0.5 full
// scale" from "cable unplugged". The rail shows normalized full scale, never
// volts — the plug-in does not know the interface's rail voltage, and printing a
// number it cannot know would be a lie the user wires into a modular.

#include "dc_processor.hpp"

#include <brew/ui/panel.hpp>

#include <cstdio>
#include <string>

namespace pulp::examples::brew {

class DcUi : public ui::BrewPanel {
public:
    explicit DcUi(state::StateStore& store)
        : ui::BrewPanel("DC", "a constant control voltage"), store_(store) {
        auto pct = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%%", v * 100.0f);
            return std::string(buf);
        };

        auto controls = ui::row(14.0f, 92.0f);
        auto value = ui::param_knob(store_, DcProcessor::kValue, "Value", [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.3f", v);
            return std::string(buf);
        });
        auto out_scale =
            ui::param_knob(store_, DcProcessor::kOutputScale, "Output Scale", pct);
        auto invert = ui::param_toggle(store_, DcProcessor::kInvert, "Invert");
        ui::fixed_size(*value, 92.0f, 92.0f);
        ui::fixed_size(*out_scale, 92.0f, 92.0f);
        // Toggle draws its label at the bottom of its own box, so the box must
        // be tall enough for both the switch and the text.
        ui::fixed_size(*invert, 78.0f, 50.0f);
        controls->add_child(std::move(value));
        controls->add_child(std::move(out_scale));
        controls->add_child(std::move(invert));

        auto readout = ui::caption_label("", 14.0f);
        readout->set_text_color(ui::palette::text);
        readout_ = readout.get();

        auto rail = std::make_unique<ui::VoltageRail>([this] { return resolved(); });
        rail->flex().preferred_height = 14.0f;
        rail->flex().align_self = view::FlexAlign::stretch;

        add_child(std::move(controls));
        add_child(std::move(readout));
        add_child(std::move(rail));
        // Full scale, never volts: the plug-in cannot know the interface's rail.
        add_child(ui::caption_label(
            "-1 . centre 0 . +1     full scale, not volts"));
    }

    void paint(canvas::Canvas& canvas) override {
        // The parent paints before its children, so the readout's text must be
        // updated here — not drawn here. A caption drawn under a child's bounds
        // is invisible.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%+.3f  full scale", resolved());
        readout_->set_text(buf);

        ui::BrewPanel::paint(canvas);

        // Parameters can move under host automation, so the editor never learns
        // of a change; repaint continuously rather than guess.
        request_repaint();
    }

    /// The value the processor is writing to every sample, after the shared
    /// output stage. Computed the same way the DSP computes it.
    float resolved() const {
        return resolve_output(store_.get_value(DcProcessor::kValue),
                              store_.get_value(DcProcessor::kOutputScale),
                              as_toggle(store_.get_value(DcProcessor::kInvert)));
    }

private:
    state::StateStore& store_;
    view::Label* readout_ = nullptr;
};

}  // namespace pulp::examples::brew
