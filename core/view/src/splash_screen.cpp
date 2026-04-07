#include <pulp/view/splash_screen.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>

namespace pulp::view {

void SplashScreen::show() {
    showing_ = true;
    elapsed_ = 0;
    opacity_ = 0;
    phase_ = Phase::FadeIn;
}

void SplashScreen::dismiss() {
    if (phase_ != Phase::Done) {
        phase_ = Phase::FadeOut;
        elapsed_ = 0;
    }
}

bool SplashScreen::advance(float dt) {
    if (!showing_) return false;

    elapsed_ += dt;

    switch (phase_) {
        case Phase::FadeIn:
            opacity_ = std::clamp(elapsed_ / fade_in_, 0.0f, 1.0f);
            if (elapsed_ >= fade_in_) {
                phase_ = Phase::Hold;
                elapsed_ = 0;
            }
            break;

        case Phase::Hold:
            opacity_ = 1.0f;
            if (elapsed_ >= duration_) {
                phase_ = Phase::FadeOut;
                elapsed_ = 0;
            }
            break;

        case Phase::FadeOut:
            opacity_ = std::clamp(1.0f - elapsed_ / fade_out_, 0.0f, 1.0f);
            if (elapsed_ >= fade_out_) {
                phase_ = Phase::Done;
                showing_ = false;
                if (on_dismissed) on_dismissed();
                return false;
            }
            break;

        case Phase::Done:
            return false;
    }

    return true;
}

void SplashScreen::paint(canvas::Canvas& canvas) {
    if (!showing_) return;

    float w = bounds().width, h = bounds().height;

    // Background (semi-transparent black)
    canvas.set_opacity(opacity_);
    canvas.set_fill_color(canvas::Color::rgba(20, 20, 25));
    canvas.fill_rect(0, 0, w, h);

    // Image
    if (!image_path_.empty()) {
        canvas.draw_image_from_file(image_path_, 0, 0, w, h);
    } else {
        // Default: centered "Loading..." text
        canvas.set_fill_color(canvas::Color::rgba(200, 200, 210));
        canvas.set_font("system", 24.0f);
        float text_w = canvas.measure_text("Loading...");
        canvas.fill_text("Loading...", (w - text_w) / 2.0f, h * 0.5f);
    }

    canvas.set_opacity(1.0f);
}

void SplashScreen::on_mouse_down(Point) {
    if (dismiss_on_click_) dismiss();
}

}  // namespace pulp::view
