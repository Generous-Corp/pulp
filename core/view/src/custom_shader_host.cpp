#include <pulp/view/custom_shader_host.hpp>

#include <pulp/canvas/canvas.hpp>

namespace pulp::view {

void CustomShaderHost::set_custom_shader(std::string sksl) {
    custom_sksl_ = std::move(sksl);
    shader_uses_time_ =
        canvas::Canvas::sksl_declares_uniform(custom_sksl_, "time");
}

void CustomShaderHost::clear_custom_shader() {
    custom_sksl_.clear();
    shader_uses_time_ = false;
}

} // namespace pulp::view
