#pragma once

/// @file lottie_animation.hpp
/// Lottie / After-Effects (Bodymovin JSON) animation playback.
///
/// Plays vector animations exported as Lottie JSON by rendering them onto a
/// Pulp Canvas. Backed by the bundled skottie module, kept entirely inside the
/// implementation (pimpl) so this header carries no rendering-backend
/// dependency and view-layer code never sees it.
///
/// Opt-in: enabled by the CMake option PULP_LOTTIE (default OFF), which defines
/// PULP_HAS_LOTTIE. When disabled, this class still exists so calling code
/// compiles unchanged, but load_*() returns false, valid() is false, and
/// render() is a no-op (supported() reports false).
///
/// All methods are control/UI-thread only; none are real-time-audio-safe.

#include <memory>
#include <string>

namespace pulp::canvas {

class Canvas;

class LottieAnimation {
public:
    LottieAnimation();
    ~LottieAnimation();
    LottieAnimation(LottieAnimation&&) noexcept;
    LottieAnimation& operator=(LottieAnimation&&) noexcept;
    LottieAnimation(const LottieAnimation&) = delete;
    LottieAnimation& operator=(const LottieAnimation&) = delete;

    /// True when Lottie support is compiled in (PULP_HAS_LOTTIE).
    static bool supported() noexcept;

    /// Load a Lottie animation from a JSON string. Returns false on parse
    /// failure or when support is not compiled in.
    bool load_json(const std::string& json);

    /// Load a Lottie animation from a .json file path.
    bool load_file(const std::string& path);

    bool valid() const noexcept;
    double duration_seconds() const noexcept;  ///< 0 when invalid.
    double frame_rate() const noexcept;         ///< 0 when invalid.
    float intrinsic_width() const noexcept;     ///< Authored composition width.
    float intrinsic_height() const noexcept;    ///< Authored composition height.

    /// Render the frame at @p t_seconds (clamped to [0, duration]) into the
    /// rectangle (x, y, w, h) on @p canvas. No-op when invalid, when w/h are
    /// non-positive, or when the canvas has no image pipeline.
    void render(Canvas& canvas, double t_seconds,
                float x, float y, float w, float h);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulp::canvas
