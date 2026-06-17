// lottie_animation.cpp — Lottie (Bodymovin JSON) playback via the bundled
// skottie module. The whole Skia/skottie dependency is confined to this TU and
// only compiled when PULP_HAS_LOTTIE is defined (CMake option PULP_LOTTIE).
// Without it, every method is a safe no-op so calling code compiles unchanged.

#include <pulp/canvas/lottie_animation.hpp>

#include <algorithm>

#if defined(PULP_HAS_LOTTIE)

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkRect.h"
#include "include/core/SkSize.h"
#include "include/core/SkSurface.h"
#include "modules/skottie/include/Skottie.h"

#include <pulp/canvas/skia_canvas.hpp>

#endif  // PULP_HAS_LOTTIE

namespace pulp::canvas {

#if defined(PULP_HAS_LOTTIE)

struct LottieAnimation::Impl {
    sk_sp<skottie::Animation> animation;
    sk_sp<SkImage> last_frame;  // keep the rendered frame alive across the draw
    double duration = 0.0;
    double fps = 0.0;
    float width = 0.0f;
    float height = 0.0f;
};

LottieAnimation::LottieAnimation() : impl_(std::make_unique<Impl>()) {}
LottieAnimation::~LottieAnimation() = default;
LottieAnimation::LottieAnimation(LottieAnimation&&) noexcept = default;
LottieAnimation& LottieAnimation::operator=(LottieAnimation&&) noexcept = default;

bool LottieAnimation::supported() noexcept { return true; }

bool LottieAnimation::load_json(const std::string& json) {
    if (!impl_) impl_ = std::make_unique<Impl>();  // self-heal a moved-from object
    auto anim = skottie::Animation::Builder().make(json.c_str(), json.size());
    if (!anim) return false;
    impl_->animation = anim;
    impl_->duration = anim->duration();
    impl_->fps = anim->fps();
    impl_->width = anim->size().width();
    impl_->height = anim->size().height();
    return true;
}

bool LottieAnimation::load_file(const std::string& path) {
    if (!impl_) impl_ = std::make_unique<Impl>();  // self-heal a moved-from object
    auto anim = skottie::Animation::Builder().makeFromFile(path.c_str());
    if (!anim) return false;
    impl_->animation = anim;
    impl_->duration = anim->duration();
    impl_->fps = anim->fps();
    impl_->width = anim->size().width();
    impl_->height = anim->size().height();
    return true;
}

bool LottieAnimation::valid() const noexcept {
    return impl_ && impl_->animation != nullptr;
}
double LottieAnimation::duration_seconds() const noexcept {
    return valid() ? impl_->duration : 0.0;
}
double LottieAnimation::frame_rate() const noexcept {
    return valid() ? impl_->fps : 0.0;
}
float LottieAnimation::intrinsic_width() const noexcept {
    return valid() ? impl_->width : 0.0f;
}
float LottieAnimation::intrinsic_height() const noexcept {
    return valid() ? impl_->height : 0.0f;
}

void LottieAnimation::render(Canvas& canvas, double t_seconds,
                             float x, float y, float w, float h) {
    if (!valid() || w <= 0.0f || h <= 0.0f) return;
    auto* skia = dynamic_cast<SkiaCanvas*>(&canvas);
    if (!skia) return;  // Lottie compositing requires the Skia canvas.

    const double dur = impl_->duration;
    const double t = std::clamp(t_seconds, 0.0, dur);
    impl_->animation->seekFrameTime(t);

    const int pw = std::max(1, static_cast<int>(w + 0.5f));
    const int ph = std::max(1, static_cast<int>(h + 0.5f));
    SkImageInfo info = SkImageInfo::MakeN32Premul(pw, ph, SkColorSpace::MakeSRGB());
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return;
    SkCanvas* c = surface->getCanvas();
    c->clear(SK_ColorTRANSPARENT);
    SkRect dst = SkRect::MakeWH(static_cast<float>(pw), static_cast<float>(ph));
    impl_->animation->render(c, &dst);

    impl_->last_frame = surface->makeImageSnapshot();
    if (impl_->last_frame) {
        skia->draw_skia_image(impl_->last_frame, x, y, w, h);
    }
}

#else  // !PULP_HAS_LOTTIE — no-op fallback so callers compile unchanged.

struct LottieAnimation::Impl {};
LottieAnimation::LottieAnimation() = default;
LottieAnimation::~LottieAnimation() = default;
LottieAnimation::LottieAnimation(LottieAnimation&&) noexcept = default;
LottieAnimation& LottieAnimation::operator=(LottieAnimation&&) noexcept = default;

bool LottieAnimation::supported() noexcept { return false; }
bool LottieAnimation::load_json(const std::string&) { return false; }
bool LottieAnimation::load_file(const std::string&) { return false; }
bool LottieAnimation::valid() const noexcept { return false; }
double LottieAnimation::duration_seconds() const noexcept { return 0.0; }
double LottieAnimation::frame_rate() const noexcept { return 0.0; }
float LottieAnimation::intrinsic_width() const noexcept { return 0.0f; }
float LottieAnimation::intrinsic_height() const noexcept { return 0.0f; }
void LottieAnimation::render(Canvas&, double, float, float, float, float) {}

#endif  // PULP_HAS_LOTTIE

}  // namespace pulp::canvas
