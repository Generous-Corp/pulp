#pragma once

// CV To OSC's editor.
//
// Two things the user cannot otherwise know: what value is being observed, and
// whether anything is actually leaving. The rail answers the first. The lamp
// answers the second — it is driven by a counter of messages the sink accepted,
// so it stays dark when Send is off, when the port is wrong, and when nothing has
// moved past the deadband. A lamp wired to the Send toggle would light in all
// three cases and tell the user nothing.

#include "cv_osc_processor.hpp"

#include <brew/ui/panel.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace pulp::examples::brew {

class CvOscUi : public ui::BrewPanel {
public:
    CvOscUi(state::StateStore& store, const CvOscProcessor& proc)
        : ui::BrewPanel("CV To OSC", "report a control voltage over the loopback"),
          store_(store), proc_(proc) {
        auto whole = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return std::string(buf);
        };
        auto hz = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f Hz", v);
            return std::string(buf);
        };
        auto fine = [](float v) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            return std::string(buf);
        };

        auto add = [&](view::View& row, state::ParamID id, const char* label,
                       std::function<std::string(float)> fmt) {
            auto k = ui::param_knob(store_, id, label, std::move(fmt));
            ui::fixed_size(*k, 88.0f, 88.0f);
            row.add_child(std::move(k));
        };

        auto controls = ui::row(12.0f, 88.0f);
        add(*controls, CvOscProcessor::kPort, "Port", whole);
        add(*controls, CvOscProcessor::kRateHz, "Rate", hz);
        add(*controls, CvOscProcessor::kDeadband, "Deadband", fine);

        auto bottom = ui::row(14.0f, 52.0f);
        auto send = ui::param_toggle(store_, CvOscProcessor::kEnabled, "Send");
        ui::fixed_size(*send, 78.0f, 50.0f);
        // Lit while the count is climbing: proof a packet left, not proof a
        // switch is on.
        auto lamp = std::make_unique<ui::Lamp>("OSC", [this] { return sending(); });
        ui::fixed_size(*lamp, 56.0f, 50.0f);
        bottom->add_child(std::move(send));
        bottom->add_child(std::move(lamp));

        auto rail = std::make_unique<ui::VoltageRail>(
            [this] { return proc_.latest(0); });
        rail->flex().preferred_height = 14.0f;
        rail->flex().align_self = view::FlexAlign::stretch;

        add_child(std::move(controls));
        add_child(std::move(bottom));
        add_child(std::move(rail));
        add_child(ui::caption_label("/brew/cv/0 and /brew/cv/1 to 127.0.0.1"));
        add_child(ui::caption_label(
            "the value is a float in [-1, +1] — full scale, not volts"));
    }

    void paint(canvas::Canvas& canvas) override {
        ui::BrewPanel::paint(canvas);
        request_repaint();
    }

private:
    /// True when the sink has taken a message since the last repaint.
    bool sending() const {
        const auto now = proc_.sent_count();
        const bool moved = now != last_count_;
        last_count_ = now;
        return moved;
    }

    state::StateStore& store_;
    const CvOscProcessor& proc_;
    mutable std::uint64_t last_count_ = 0;
};

}  // namespace pulp::examples::brew
