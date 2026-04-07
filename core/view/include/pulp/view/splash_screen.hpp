#pragma once

// SplashScreen — borderless window with image and fade animation.
// Shows on app launch, dismisses on click or after a timeout.

#include <pulp/view/view.hpp>
#include <pulp/view/animation.hpp>
#include <string>
#include <functional>
#include <chrono>

namespace pulp::view {

class SplashScreen : public View {
public:
    SplashScreen() = default;

    /// Set the splash image path (PNG, JPEG)
    void set_image(std::string_view path) { image_path_ = std::string(path); }

    /// Set how long the splash is visible (default: 3 seconds)
    void set_duration(float seconds) { duration_ = seconds; }

    /// Set fade-in duration (default: 0.3s)
    void set_fade_in(float seconds) { fade_in_ = seconds; }

    /// Set fade-out duration (default: 0.5s)
    void set_fade_out(float seconds) { fade_out_ = seconds; }

    /// Whether to dismiss on click (default: true)
    void set_dismiss_on_click(bool dismiss) { dismiss_on_click_ = dismiss; }

    /// Show the splash screen. Blocks until dismissed or timed out.
    void show();

    /// Dismiss the splash screen immediately.
    void dismiss();

    /// Whether the splash is currently showing.
    bool is_showing() const { return showing_; }

    /// Called when the splash is dismissed.
    std::function<void()> on_dismissed;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

    /// Advance the animation by dt seconds. Returns false when done.
    bool advance(float dt);

private:
    std::string image_path_;
    float duration_ = 3.0f;
    float fade_in_ = 0.3f;
    float fade_out_ = 0.5f;
    bool dismiss_on_click_ = true;
    bool showing_ = false;

    float elapsed_ = 0.0f;
    float opacity_ = 0.0f;

    enum class Phase { FadeIn, Hold, FadeOut, Done };
    Phase phase_ = Phase::FadeIn;
};

}  // namespace pulp::view
