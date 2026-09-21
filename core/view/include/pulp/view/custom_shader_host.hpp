#pragma once

#include <string>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <pulp/canvas/canvas.hpp>

namespace pulp::view {

/// Mixin for widgets whose body can be painted by a custom SkSL shader.
///
/// The shader replaces the widget's body / track / fill drawing only — labels,
/// value text, and hover glow keep painting in C++. Widgets opt in by
/// inheriting this alongside `View`, which is what makes shader support
/// *discoverable*: the JS bridge locates shader-capable widgets with a single
/// `dynamic_cast<CustomShaderHost*>` rather than a hardcoded chain of concrete
/// widget types.
///
/// The render loop discovers this mixin too. Core shader widgets use an
/// allocation-free type tag on their hot path; any other future widget falls
/// back to this mixin, including a subclass that inherits a non-shader tag, so
/// a shader declaring a `time` uniform still keeps its view tree live.
class CustomShaderHost {
public:
    struct ShaderValueBinding {
        std::string uniform_name;
        std::string channel_name;
        std::string param_name;
        std::uint32_t last_publish_seq = 0;
        std::chrono::steady_clock::time_point last_publish_at{};
        float neutral = 0.0f;
    };
    struct ShaderScopeBinding {
        std::string channel_name;
        std::shared_ptr<const canvas::Canvas::ShaderDataTexture> data;
        std::uint32_t last_publish_seq = 0;
        std::chrono::steady_clock::time_point last_publish_at{};
        float neutral = 0.0f;
    };
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
    void set_chart_shader(std::string sksl) { chart_sksl_ = std::move(sksl); }
    const std::string& chart_shader() const { return chart_sksl_; }

    void set_shader_uniforms(std::vector<canvas::Canvas::NamedUniform> uniforms) {
        shader_uniforms_ = std::move(uniforms);
    }
    const std::vector<canvas::Canvas::NamedUniform>& shader_uniforms() const { return shader_uniforms_; }
    void set_shader_value_bindings(std::vector<ShaderValueBinding> bindings) {
        shader_value_bindings_ = std::move(bindings);
    }
    const std::vector<ShaderValueBinding>& shader_value_bindings() const {
        return shader_value_bindings_;
    }
    std::vector<ShaderValueBinding>& shader_value_bindings() { return shader_value_bindings_; }
    bool shader_uniform_bound(const std::string& name) const {
        return std::any_of(shader_value_bindings_.begin(), shader_value_bindings_.end(),
                           [&](const auto& binding) { return binding.uniform_name == name; });
    }
    void set_shader_scope_binding(std::optional<ShaderScopeBinding> binding) {
        shader_scope_binding_ = std::move(binding);
    }
    const std::optional<ShaderScopeBinding>& shader_scope_binding() const {
        return shader_scope_binding_;
    }
    std::optional<ShaderScopeBinding>& shader_scope_binding() { return shader_scope_binding_; }
    bool set_shader_uniform_value(const std::string& name, float value) {
        for (auto& uniform : shader_uniforms_) {
            if (uniform.name == name && uniform.count == 1) {
                if (uniform.v[0] == value) return false;
                uniform.v[0] = value;
                return true;
            }
        }
        canvas::Canvas::NamedUniform uniform;
        uniform.name = name;
        uniform.count = 1;
        uniform.v[0] = value;
        shader_uniforms_.push_back(std::move(uniform));
        return true;
    }
    void set_shader_reach(float reach) { shader_reach_ = reach < 0.0f ? 0.0f : reach; }
    float shader_reach() const { return shader_reach_; }
    void set_shader_geometry(std::optional<canvas::Canvas::ShaderGeometry> geometry) {
        shader_geometry_ = std::move(geometry);
    }
    const std::optional<canvas::Canvas::ShaderGeometry>& shader_geometry() const {
        return shader_geometry_;
    }
    void set_shader_geometry_spec(std::string spec, std::uint64_t topology_hash) {
        shader_geometry_spec_ = std::move(spec);
        shader_geometry_topology_hash_ = topology_hash;
    }
    const std::string& shader_geometry_spec() const { return shader_geometry_spec_; }
    std::uint64_t shader_geometry_topology_hash() const { return shader_geometry_topology_hash_; }

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
    std::string chart_sksl_;
    bool shader_uses_time_ = false;
    bool shader_draw_failure_logged_ = false;
    std::vector<canvas::Canvas::NamedUniform> shader_uniforms_;
    std::vector<ShaderValueBinding> shader_value_bindings_;
    std::optional<ShaderScopeBinding> shader_scope_binding_;
    float shader_reach_ = 0.0f;
    std::optional<canvas::Canvas::ShaderGeometry> shader_geometry_;
    std::string shader_geometry_spec_;
    std::uint64_t shader_geometry_topology_hash_ = 0;
};

} // namespace pulp::view
