// lottie_view.cpp — FrameClock-driven Lottie playback widget. See
// pulp/view/lottie_view.hpp. Rendering is delegated to canvas::LottieAnimation;
// this file only owns the playhead, frame-clock subscription, and reduced-motion
// gating.

#include <pulp/view/lottie_view.hpp>

#include <cmath>

#include <pulp/canvas/lottie_animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion_preferences.hpp>

namespace pulp::view {

LottieView::LottieView()
    : animation_(std::make_unique<canvas::LottieAnimation>()) {}

LottieView::~LottieView() { unsubscribe_clock(); }

bool LottieView::supported() { return canvas::LottieAnimation::supported(); }

bool LottieView::set_source_file(const std::string& path) {
    const bool ok = animation_->load_file(path);
    time_ = 0.0;
    if (ok) {
        subscribe_clock();
        request_repaint();
    }
    return ok;
}

bool LottieView::set_source_json(const std::string& json) {
    const bool ok = animation_->load_json(json);
    time_ = 0.0;
    if (ok) {
        subscribe_clock();
        request_repaint();
    }
    return ok;
}

bool LottieView::valid() const { return animation_ && animation_->valid(); }

double LottieView::duration() const {
    return animation_ ? animation_->duration_seconds() : 0.0;
}

void LottieView::set_playing(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    if (playing_) {
        subscribe_clock();
    }
    request_repaint();
}

void LottieView::seek(double seconds) {
    const double dur = duration();
    time_ = dur > 0.0 ? std::clamp(seconds, 0.0, dur) : 0.0;
    request_repaint();
}

void LottieView::advance(float dt) {
    if (!playing_ || !valid()) return;
    // Respect the reduced-motion "off" policy: hold the current frame.
    if (motion_policy_is_off()) return;

    const double dur = duration();
    if (dur <= 0.0) return;

    time_ += static_cast<double>(dt) * static_cast<double>(speed_);
    if (time_ >= dur) {
        if (looping_) {
            time_ = std::fmod(time_, dur);
        } else {
            time_ = dur;
            playing_ = false;
        }
    }
    request_repaint();
}

void LottieView::paint(canvas::Canvas& canvas) {
    if (!valid()) return;
    const Rect b = local_bounds();
    animation_->render(canvas, time_, b.x, b.y, b.width, b.height);
}

void LottieView::on_attached() { subscribe_clock(); }

void LottieView::on_detached() { unsubscribe_clock(); }

void LottieView::subscribe_clock() {
    if (clock_subscription_ >= 0) return;
    if (!playing_ || !valid()) return;
    if (motion_policy_is_off()) return;  // static frame; no ticking needed
    FrameClock* clock = frame_clock();
    if (!clock) return;
    clock_subscription_ = clock->subscribe([this](float dt) {
        advance(dt);
        // Keep the subscription while actively playing; drop it otherwise so an
        // idle/finished animation costs nothing. When the clock auto-removes us
        // (return false), clear the stored id so a later play/resume/seek can
        // resubscribe — otherwise the >=0 guard in subscribe_clock() would block
        // it forever (frozen animation after the first pause or non-loop end).
        const bool keep = playing_ && valid();
        if (!keep) clock_subscription_ = -1;
        return keep;
    });
}

void LottieView::unsubscribe_clock() {
    if (clock_subscription_ < 0) return;
    if (FrameClock* clock = frame_clock()) {
        clock->unsubscribe(clock_subscription_);
    }
    clock_subscription_ = -1;
}

}  // namespace pulp::view
