#pragma once

#include "pulp_delay_ui_palette.hpp"

#include <pulp/state/listener_token.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/widgets.hpp>

#include <string>
#include <vector>

namespace pulp::examples::delay::ui {

std::string format_parameter_value(const state::StateStore& store,
                                   state::ParamID id,
                                   float normalized);

class DelayParameterControl {
  public:
    virtual ~DelayParameterControl() = default;
    virtual state::ParamID parameter_id() const noexcept = 0;
    virtual float normalized_value() const noexcept = 0;
    virtual void sync_from_store(state::StateStore& store) = 0;
};

class DelayKnob final : public view::Knob, public DelayParameterControl {
  public:
    struct DialGeometry {
        float center_x = 0.0f;
        float center_y = 0.0f;
        float body_radius = 0.0f;
        float arc_radius = 0.0f;
    };

    DelayKnob(state::StateStore& store, const CharacterPalette& palette,
              state::ParamID id, std::string caption);

    state::ParamID parameter_id() const noexcept override { return id_; }
    float normalized_value() const noexcept override { return value(); }
    void sync_from_store(state::StateStore& store) override;
    float pointer_angle() const noexcept;
    DialGeometry dial_geometry() const noexcept;
    std::string display_text() const;

    void paint(canvas::Canvas& canvas) override;
    void on_focus_changed(bool gained) override;

  private:
    state::StateStore* store_ = nullptr;
    const CharacterPalette* palette_ = nullptr;
    state::ParamID id_ = 0;
    std::string caption_;
};

class DelayFader : public view::Fader, public DelayParameterControl {
  public:
    DelayFader(state::StateStore& store, const CharacterPalette& palette,
               state::ParamID id, std::string caption);

    state::ParamID parameter_id() const noexcept override { return id_; }
    float normalized_value() const noexcept override { return value(); }
    void sync_from_store(state::StateStore& store) override;
    std::string display_text() const;

    void paint(canvas::Canvas& canvas) override;
    void on_focus_changed(bool gained) override;

  protected:
    state::StateStore* store_ = nullptr;
    const CharacterPalette* palette_ = nullptr;

  private:
    state::ParamID id_ = 0;
    std::string caption_;
};

class DelayTapField final : public DelayFader {
  public:
    DelayTapField(state::StateStore& store,
                  const CharacterPalette& palette,
                  state::ParamID time_id,
                  state::ParamID feedback_id,
                  state::ParamID sync_id,
                  state::ParamID division_id);

    std::string timing_display_text() const;
    void paint(canvas::Canvas& canvas) override;

  private:
    state::ParamID feedback_id_ = 0;
    state::ParamID sync_id_ = 0;
    state::ParamID division_id_ = 0;
    state::ListenerToken feedback_listener_;
};

class DelayChoice final : public view::View, public DelayParameterControl {
  public:
    DelayChoice(state::StateStore& store,
                const CharacterPalette& palette,
                state::ParamID id,
                std::string caption,
                std::vector<std::string> labels);

    state::ParamID parameter_id() const noexcept override { return id_; }
    float normalized_value() const noexcept override;
    void sync_from_store(state::StateStore& store) override;
    int selected_index() const noexcept;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(view::Point position) override;
    void on_focus_changed(bool gained) override;
    bool wants_mouse_input() const override { return true; }
    void layout_children() override {}

  private:
    bool sync_access_value();

    state::StateStore* store_ = nullptr;
    const CharacterPalette* palette_ = nullptr;
    state::ParamID id_ = 0;
    std::string caption_;
    std::vector<std::string> labels_;
    state::ListenerToken listener_;
};

class DelayActionCard final : public view::ToggleButton,
                              public DelayParameterControl {
  public:
    DelayActionCard(const CharacterPalette& palette,
                    state::ParamID id,
                    std::string caption,
                    std::string subtitle,
                    std::string glyph,
                    bool compact = false,
                    bool warning = false);

    state::ParamID parameter_id() const noexcept override { return id_; }
    float normalized_value() const noexcept override { return is_on() ? 1.0f : 0.0f; }
    void sync_from_store(state::StateStore& store) override;

    void paint(canvas::Canvas& canvas) override;
    void on_focus_changed(bool gained) override;

  private:
    const CharacterPalette* palette_ = nullptr;
    state::ParamID id_ = 0;
    std::string caption_;
    std::string subtitle_;
    std::string glyph_;
    bool compact_ = false;
    bool warning_ = false;
};

} // namespace pulp::examples::delay::ui
