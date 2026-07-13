# SuperConvolver DSP regression tests (offline/bounce fidelity, Size headroom,
# Flow audibility, IR sources + state). The processor lives in
# examples/super-convolver, so this links the example's view TU for the processor
# vtable (create_view()) and adds the example dir to the include path — mirroring
# examples/super-convolver's own super-convolver-test target. GPU-path assertions
# skip cleanly with no device.
if(PULP_BUILD_TESTS AND TARGET pulp::gpu-audio AND TARGET pulp::render)
    pulp_add_test_suite(pulp-test-super-convolver-dsp
        SOURCES test_super_convolver_dsp.cpp
                harness/rt_allocation_probe.cpp
                ${CMAKE_SOURCE_DIR}/examples/super-convolver/super_convolver_view.cpp
        LIBRARIES pulp::format pulp::signal pulp::gpu-audio pulp::render
                  pulp::view pulp::canvas
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/super-convolver)
endif()

# CLAP parameter-display coverage: the units SuperConvolver declares only reach a
# host through clap_plugin_params.value_to_text (clap_param_info has no unit
# field), and the WAM / WebCLAP demo pages assert the same golden strings. Needs
# the CLAP adapter TUs + headers, hence its own target rather than the DSP one.
if(PULP_BUILD_TESTS AND PULP_HAS_CLAP AND TARGET pulp::gpu-audio AND TARGET pulp::render)
    pulp_add_test_suite(pulp-test-super-convolver-clap-params
        SOURCES test_super_convolver_clap_params.cpp
                ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
                ${CMAKE_SOURCE_DIR}/core/format/src/clap_remote_controls.cpp
                ${CMAKE_SOURCE_DIR}/examples/super-convolver/super_convolver_view.cpp
        LIBRARIES pulp::format pulp::signal pulp::gpu-audio pulp::render
                  pulp::view pulp::canvas clap
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/super-convolver)
endif()

# The BROWSER GPU engine, tested natively. The engine's only route to a GPU is the
# `pulp_gpu_xfer` wasm import, so compiling the processor with PULP_WASM +
# PULP_WEB_GPU_AUDIO and linking a stub import in the test IS the browser, minus
# the nondeterminism — the whole engine (latency, safety-net alignment, miss and
# hit paths) is decided on this side of that seam and needs no browser and no GPU.
# Only pulp::format + pulp::signal are linked (as in the web lanes' own link line):
# if the GPU/render stack ever leaked back into the web gating, this fails to
# build. PULP_HEADLESS drops create_view(), so no view TU is needed.
#
# The same source is built a SECOND time WITHOUT PULP_WEB_GPU_AUDIO to pin the
# other half of the contract — the shipped CPU-only web module still declares
# exactly four parameters. It is a separate binary because one binary cannot hold
# both configurations of the class (different layouts → ODR).
if(PULP_BUILD_TESTS)
    pulp_add_test_suite(pulp-test-super-convolver-web-gpu
        SOURCES test_super_convolver_web_gpu.cpp
        LIBRARIES pulp::format pulp::signal
        COMPILE_DEFINITIONS PULP_WASM=1 PULP_HEADLESS=1 PULP_WEB_GPU_AUDIO=1
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/super-convolver)

    pulp_add_test_suite(pulp-test-super-convolver-web-cpu
        SOURCES test_super_convolver_web_gpu.cpp
        LIBRARIES pulp::format pulp::signal
        COMPILE_DEFINITIONS PULP_WASM=1 PULP_HEADLESS=1
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
