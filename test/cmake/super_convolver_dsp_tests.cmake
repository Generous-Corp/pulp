# SuperConvolver DSP regression tests (offline/bounce fidelity, Size headroom,
# Flow audibility, IR sources + state). The processor lives in
# examples/super-convolver, so this links the example's view TU for the processor
# vtable (create_view()) and adds the example dir to the include path — mirroring
# examples/super-convolver's own super-convolver-test target. GPU-path assertions
# skip cleanly with no device.
if(PULP_BUILD_TESTS AND TARGET pulp::gpu-audio AND TARGET pulp::render)
    pulp_add_test_suite(pulp-test-super-convolver-dsp
        SOURCES test_super_convolver_dsp.cpp
                ${CMAKE_SOURCE_DIR}/examples/super-convolver/super_convolver_view.cpp
        LIBRARIES pulp::format pulp::signal pulp::gpu-audio pulp::render
                  pulp::view pulp::canvas
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/super-convolver)
endif()

# Web-lane compile guard. The processor must still compile with PULP_WASM +
# PULP_HEADLESS — the two web plugin lanes (WAM, WCLAP), where there is no GPU
# runtime, no file dialog, no FormatRegistry, no editor, and no thread to run a
# background worker on. The guard TU is compiled against ONLY pulp::format +
# pulp::signal, so the gpu_audio headers are not even on its include path: if the
# gating regressed, this fails to compile. It exercises the web-lane API surface
# (built-in IRs, set_ir_pcm, service_ir_rebuild, the SCv2 state blob) so those
# stay instantiable there, and is compile-only — it links nothing.
if(PULP_BUILD_TESTS)
    set(_pulp_sc_web_guard
        ${CMAKE_CURRENT_BINARY_DIR}/super_convolver_web_guard.cpp)
    file(WRITE ${_pulp_sc_web_guard} [==[
#include "super_convolver.hpp"

#include <cstdint>
#include <span>
#include <vector>

static_assert(pulp::examples::kBuiltInIrCount >= 3,
              "the web demo needs a family of built-in IRs to switch between");

void pulp_super_convolver_web_guard();
void pulp_super_convolver_web_guard() {
    pulp::examples::SuperConvolverProcessor proc;
    proc.set_background_worker_enabled(false);
    proc.set_built_in_ir(1);
    const std::vector<float> pcm(64, 0.0f);
    proc.set_ir_pcm(pcm.data(), pcm.size(), 1, 48000.0, "guard");
    proc.service_ir_rebuild();
    const std::vector<std::uint8_t> blob = proc.serialize_plugin_state();
    proc.deserialize_plugin_state(std::span<const std::uint8_t>(blob));
    (void)proc.gpu_engine_active();
    (void)pulp::examples::make_builtin_ir(2, 16);
}
]==])
    add_library(pulp-super-convolver-web-guard OBJECT ${_pulp_sc_web_guard})
    target_compile_definitions(pulp-super-convolver-web-guard
        PRIVATE PULP_WASM=1 PULP_HEADLESS=1)
    target_link_libraries(pulp-super-convolver-web-guard
        PRIVATE pulp::format pulp::signal)
    target_include_directories(pulp-super-convolver-web-guard
        PRIVATE ${CMAKE_SOURCE_DIR}/examples/super-convolver)
endif()
