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

private:
    std::string custom_sksl_;
    bool shader_uses_time_ = false;
};

} // namespace pulp::view
