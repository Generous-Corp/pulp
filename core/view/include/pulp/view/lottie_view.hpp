#pragma once

/// @file lottie_view.hpp
/// A view widget that plays a Lottie / After-Effects (Bodymovin JSON) animation.
///
/// Thin time-driver over canvas::LottieAnimation: it advances a playhead from
/// the view system's FrameClock, honors the reduced-motion preference, and
/// paints the current frame onto the canvas. All rendering lives in
/// canvas::LottieAnimation, so this header carries no rendering-backend
/// dependency.
///
/// Lottie playback is opt-in (CMake PULP_LOTTIE). When it is not compiled in,
/// the widget still constructs and lays out, but load_* return false and paint
/// draws nothing — calling code is unaffected.

#include <memory>
#include <string>

#include <pulp/view/view.hpp>

namespace pulp::canvas { class LottieAnimation; }

namespace pulp::view {

class LottieView : public View {
public:
    LottieView();
    ~LottieView() override;

    /// True when Lottie support is compiled in (PULP_LOTTIE / PULP_HAS_LOTTIE).
    static bool supported();

    bool set_source_file(const std::string& path);
    bool set_source_json(const std::string& json);
    bool valid() const;

    void set_playing(bool playing);
    bool playing() const { return playing_; }
    void set_looping(bool looping) { looping_ = looping; }
    bool looping() const { return looping_; }
    void set_speed(float speed) { speed_ = speed < 0.0f ? 0.0f : speed; }
    float speed() const { return speed_; }

    void seek(double seconds);
    double time() const { return time_; }
    double duration() const;

    /// Advance the playhead by @p dt seconds (scaled by speed, looped/clamped).
    /// Called automatically from the FrameClock; exposed for headless tests.
    /// Honors the reduced-motion "off" policy by not advancing.
    void advance(float dt);

    void paint(canvas::Canvas& canvas) override;
    void on_attached() override;
    void on_detached() override;

private:
    void subscribe_clock();
    void unsubscribe_clock();
    // Paint a bordered "Lottie unavailable" placeholder when Lottie is compiled
    // out of this build, so a disabled build reads as intentionally-unavailable
    // rather than a silently blank box (mirrors the ImageView missing-image
    // placeholder). Logs the reason once per view instance.
    void paint_unsupported_placeholder(canvas::Canvas& canvas);

    std::unique_ptr<canvas::LottieAnimation> animation_;
    double time_ = 0.0;
    float speed_ = 1.0f;
    bool playing_ = true;
    bool looping_ = true;
    int clock_subscription_ = -1;
    bool unsupported_logged_ = false;
};

}  // namespace pulp::view
