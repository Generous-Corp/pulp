#pragma once

// CV To OSC's editor.
//
// Three things the user cannot otherwise know: where the messages are going, what
// value is being observed, and whether anything is actually leaving. The Target
// and Path fields answer the first. The rails answer the second. The lamps answer
// the third — each is driven by a counter of messages that channel's sink
// accepted, so a lamp stays dark when the channel is off, when the destination is
// wrong, and when nothing has moved past the threshold. A lamp wired to the Enable
// toggle would light in all three cases and tell the user nothing.
//
// Browse asks the network which receivers exist rather than making the user find
// out. When the platform ships no mDNS backend it says so, because "found no
// receivers" and "this build cannot look" are different facts and only one of them
// is the user's problem.

#include "cv_osc_discovery.hpp"
#include "cv_osc_processor.hpp"

#include <brew/ui/panel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::examples::brew {

/// How many discovered receivers the editor offers at once. A studio LAN with
/// more than a handful of OSC listeners is not a case worth growing the window
/// for, and the Target field still accepts anything typed.
inline constexpr std::size_t kBrowseSlots = 3;

class CvOscUi : public ui::BrewPanel {
public:
    /// Height of one discovered-receiver button.
    static constexpr float kSlotHeight = 26.0f;

    /// What the status line says before anyone presses Browse. The slot row is
    /// reserved and empty at rest, and an empty strip of panel reads as a bug;
    /// naming what would fill it reads as an offer.
    static constexpr const char* kBrowseHint =
        "Browse finds OSC receivers announced on this network";

    CvOscUi(state::StateStore& store, CvOscProcessor& proc)
        : ui::BrewPanel("CV To OSC", "report a control voltage over OSC"),
          store_(store), proc_(proc) {
        const OscSettings settings = proc_.osc_settings();

        // ── Destination ──────────────────────────────────────────────────────
        auto target = std::make_unique<ui::TextField>(
            "Target (host:port)", format_osc_target(settings.target),
            [this](const std::string& t) { return proc_.set_target(t); });
        target->flex().flex_grow = 1;
        target->flex().flex_shrink = 1;
        target_ = target.get();

        auto browse = std::make_unique<view::TextButton>("Browse");
        browse->on_click = [this] { toggle_browse(); };
        ui::fixed_size(*browse, 76.0f, ui::TextField::kFieldHeight);

        auto destination = ui::row(ui::kRowGap, ui::TextField::kFieldHeight);
        destination->add_child(std::move(target));
        destination->add_child(std::move(browse));
        add_child(std::move(destination));

        // The discovery list. Present but hidden until Browse finds something,
        // rather than grown and shrunk: a panel that changes height while the user
        // is reading it moves the control they were reaching for.
        auto found = ui::row(ui::kRowGap, kSlotHeight);
        for (std::size_t i = 0; i < kBrowseSlots; ++i) {
            auto slot = std::make_unique<view::TextButton>("");
            slot->set_style(view::TextButton::Style::ghost);
            slot->on_click = [this, i] { use_receiver(i); };
            slot->set_visible(false);
            ui::fixed_size(*slot, 108.0f, kSlotHeight);
            slots_[i] = slot.get();
            found->add_child(std::move(slot));
        }
        add_child(std::move(found));

        auto status = std::make_unique<view::Label>();
        status->set_font_size(10.0f);
        status->set_text_color(ui::palette::text_dim);
        status->flex().preferred_height = 12.0f;
        status->set_text(kBrowseHint);
        status_ = status.get();
        add_child(std::move(status));

        // ── The rate, once, because one thread sends both channels ────────────
        auto global = ui::row(ui::kRowGap, ui::kKnobHeight);
        auto rate = ui::param_knob(store_, CvOscProcessor::kRateHz, "Rate");
        ui::knob_size(*rate);
        global->add_child(std::move(rate));
        add_child(std::move(global));

        // ── Per channel ──────────────────────────────────────────────────────
        for (std::size_t ch = 0; ch < kOscChannels; ++ch) {
            add_child(ui::channel_label(ch == 0 ? "LEFT" : "RIGHT"));

            auto path = std::make_unique<ui::TextField>(
                "OSC Path", settings.paths[ch],
                [this, ch](const std::string& p) { return proc_.set_path(ch, p); });
            path->flex().align_self = view::FlexAlign::stretch;

            auto controls = ui::row(ui::kRowGap, ui::kKnobHeight);
            auto threshold = ui::param_knob(
                store_,
                static_cast<state::ParamID>(param_for(CvOscProcessor::kThreshold, ch)),
                "Threshold");
            ui::knob_size(*threshold);
            auto enable = ui::param_toggle(
                store_,
                static_cast<state::ParamID>(param_for(CvOscProcessor::kEnable, ch)),
                "Enable");
            ui::toggle_size(*enable);
            // Lit while this channel's count is climbing: proof a packet left, not
            // proof a switch is on.
            auto lamp =
                std::make_unique<ui::Lamp>("OSC", [this, ch] { return sending(ch); });
            ui::fixed_size(*lamp, 46.0f, ui::kToggleHeight);

            controls->add_child(std::move(threshold));
            controls->add_child(std::move(enable));
            controls->add_child(std::move(lamp));

            auto rail = std::make_unique<ui::VoltageRail>(
                [this, ch] { return proc_.latest(ch); });
            rail->flex().preferred_height = 14.0f;
            rail->flex().align_self = view::FlexAlign::stretch;

            add_child(std::move(path));
            add_child(std::move(controls));
            add_child(std::move(rail));
        }

        add_child(ui::caption_label(
            "the value is a float in [-1, +1] — full scale, not volts"));
        add_child(ui::caption_label("Return commits a field, Escape reverts it"));
    }

    void paint(canvas::Canvas& canvas) override {
        poll_discovery();
        ui::BrewPanel::paint(canvas);
        request_repaint();
    }

    /// Pull a snapshot of the browse results into the buttons. Runs on the UI
    /// thread from `paint`, never on the discovery callback's thread — which is
    /// why the buttons are touched here and not where a service is found.
    /// Public so a test can advance the editor without a canvas.
    void poll_discovery() {
        offered_ = discovery_.running() ? discovery_.receivers()
                                        : std::vector<DiscoveredReceiver>{};
        for (std::size_t i = 0; i < kBrowseSlots; ++i) {
            const bool has = i < offered_.size();
            slots_[i]->set_visible(has);
            if (has) slots_[i]->set_label(offered_[i].name);
        }
        if (discovery_.running())
            status_->set_text(offered_.empty()
                                  ? "browsing for " + std::string(kOscServiceType) + "…"
                                  : "click a receiver to use it");
    }

    /// The receivers the editor is currently offering. Exposed so a test can
    /// assert what Browse surfaced without reading a button's label.
    [[nodiscard]] const std::vector<DiscoveredReceiver>& offered() const noexcept {
        return offered_;
    }

    /// Fill the Target field from a discovered receiver, as clicking its button
    /// does. Separated from the click handler so a test can drive it headlessly.
    void use_receiver(std::size_t slot) {
        if (slot >= offered_.size()) return;
        const std::string text = format_osc_target(offered_[slot].target);
        if (proc_.set_target(text)) target_->set_accepted(text);
    }

    /// Start or stop discovery. Exposed for the same reason.
    void toggle_browse() {
        if (discovery_.running()) {
            discovery_.stop();
            status_->set_text(kBrowseHint);
        } else if (!discovery_.start()) {
            // Honest, and different from "found nothing": this build has no way to
            // look, so no amount of waiting will populate the list.
            status_->set_text("no mDNS backend on this platform — type a target");
        }
        poll_discovery();
    }

    OscDiscovery& discovery() noexcept { return discovery_; }

private:
    /// True when this channel's sink has taken a message since the last repaint.
    bool sending(std::size_t ch) const {
        const auto now = proc_.sent_count(ch);
        const bool moved = now != last_count_[ch];
        last_count_[ch] = now;
        return moved;
    }

    state::StateStore& store_;
    CvOscProcessor& proc_;
    OscDiscovery discovery_;

    ui::TextField* target_ = nullptr;
    view::Label* status_ = nullptr;
    std::array<view::TextButton*, kBrowseSlots> slots_{};
    std::vector<DiscoveredReceiver> offered_;

    mutable std::array<std::uint64_t, kOscChannels> last_count_{};
};

}  // namespace pulp::examples::brew
