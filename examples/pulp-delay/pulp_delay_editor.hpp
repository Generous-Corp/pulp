#pragma once

#include "delay_params.hpp"
#include "pulp_delay_controls.hpp"

#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/view.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::examples::delay::ui {

class PulpDelayEditor final : public view::View {
  public:
    explicit PulpDelayEditor(state::StateStore& store);

    DelayParameterControl* control_for(state::ParamID id) const noexcept;
    std::size_t bound_parameter_count() const noexcept;
    state::StateStore& parameter_store() const noexcept { return *store_; }
    static constexpr std::array<std::string_view, 5> truthful_header_labels() noexcept {
        return {"AUTHORED CONTROL SURFACE", "LIVE PARAMETER BINDINGS", "NATIVE VIEW",
                "25 PARAMS", "SKIA CANVAS"};
    }
    float control_state_level(state::ParamID id) const noexcept;
    void sync_from_store();

    void paint(canvas::Canvas& canvas) override;
    void layout_children() override {}

  private:
    class Panel;

    Panel& add_panel(view::Rect bounds, int index, std::string title);
    DelayKnob& add_knob(view::View& parent, view::Rect bounds,
                        state::ParamID id, std::string caption);
    DelayFader& add_fader(view::View& parent, view::Rect bounds,
                          state::ParamID id, std::string caption);
    DelayTapField& add_tap_field(view::View& parent, view::Rect bounds);
    DelayChoice& add_choice(view::View& parent, view::Rect bounds,
                            state::ParamID id, std::string caption,
                            std::vector<std::string> labels);
    DelayActionCard& add_action(view::View& parent, view::Rect bounds,
                                state::ParamID id, std::string caption,
                                std::string subtitle, std::string glyph,
                                bool compact = false);
    void register_control(DelayParameterControl& control);
    void build();

    state::StateStore* store_ = nullptr;
    std::array<DelayParameterControl*, kParameterCount> controls_{};
    std::vector<view::ParameterBinding> bindings_;
};

std::unique_ptr<view::View> build_pulp_delay_editor(state::StateStore& store);

} // namespace pulp::examples::delay::ui
