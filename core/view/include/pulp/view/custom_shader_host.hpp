#pragma once

#include <string>

namespace pulp::view {

/// Mixin for widgets whose body can be painted by a custom SkSL shader.
///
/// The shader replaces the widget's body / track / fill drawing only — labels,
/// value text, and hover glow keep painting in C++. Widgets opt in by
/// inheriting this alongside `View`, which is what makes shader support
/// *discoverable*: the JS bridge and the continuous-frame check each locate
/// shader-capable widgets with a single `dynamic_cast<CustomShaderHost*>`
/// rather than a hardcoded chain of concrete widget types. A new shader-capable
/// widget therefore needs no edits outside its own class.
class CustomShaderHost {
public:
    virtual ~CustomShaderHost() = default;

    /// Install an SkSL body shader.
    ///
    /// Whether the shader is time-driven is resolved once, here — see
    /// `shader_uses_time()`.
    void set_custom_shader(std::string sksl);

    /// Remove the shader and restore the default C++ paint path.
    void clear_custom_shader();

    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }

    /// True when the shader actually declares a `time` uniform, and therefore
    /// needs a continuous repaint to animate.
    ///
    /// Resolved when the shader is installed, not on each call: this is read
    /// once per widget per frame by `needs_continuous_frames()`, so it must not
    /// recompile or rescan. It is a real uniform lookup on the compiled effect,
    /// not a substring search — a shader with a `timeline` uniform must not pin
    /// the render loop to 120 Hz, and one that spells its uniform differently
    /// must not silently freeze.
    bool shader_uses_time() const { return shader_uses_time_; }

    /// One-shot latch for "the body shader failed to draw and we logged it".
    /// A shader can fail at DRAW time (not just install time) — e.g. the runtime
    /// effect compiles but the backend can't produce a shader — and the widget
    /// paint path must then fall back to its C++ body instead of leaving the
    /// widget blank. The paint path logs that once per host instance (guarded by
    /// this latch) so it does not spam a line every frame.
    bool shader_draw_failure_logged() const { return shader_draw_failure_logged_; }
    void mark_shader_draw_failure_logged() { shader_draw_failure_logged_ = true; }

private:
    std::string custom_sksl_;
    bool shader_uses_time_ = false;
    bool shader_draw_failure_logged_ = false;
};

} // namespace pulp::view
