add_library(pulp-gpu-audio)
add_library(pulp::gpu-audio ALIAS pulp-gpu-audio)

target_include_directories(pulp-gpu-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # The public GPU-node headers (gpu_convolver.hpp, gpu_stft.hpp, ...)
        # render::GpuCompute, so they #include <pulp/render/gpu_compute.hpp>. Those
        # render headers are Dawn-free (opaque void* device/queue handles), so
        # exposing render's include dir keeps the gpu_audio public headers
        # self-contained even in a GPU-off configure where pulp::render isn't
        # linked (the transport is still built there). $<INSTALL_INTERFACE:include>
        # already covers render's headers in the installed SDK.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/render/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-gpu-audio
    PRIVATE
        src/gpu_audio_transport.cpp
)

target_compile_features(pulp-gpu-audio PUBLIC cxx_std_20)

# GPU-agnostic real-time transport: depends only on the audio buffer/ring
# primitives and the lock-free runtime. The public convolver classes are also
# defined in GPU-off builds so examples and SDK consumers can compile the same
# source and observe gpu_available() == false.
find_package(Threads REQUIRED)
target_link_libraries(pulp-gpu-audio
    PUBLIC
        pulp::audio
        pulp::runtime
    PRIVATE
        Threads::Threads
)

# The GPU convolver node uses render::GpuCompute (FFT) + signal::Convolver (CPU
# fallback). Without pulp::render, compile no-render definitions that satisfy the
# public API and let callers fall back cleanly at runtime.
if(TARGET pulp::render)
    # src/web/web_gpu_convolver.cpp is the submit/collect (never-blocking)
    # counterpart of the GpuConvolver node, written for a single-threaded event
    # loop. It is plain C++ over the GpuCompute interface, so it builds and is
    # unit-tested natively here even though its reason to exist is the browser
    # worker (which compiles the same TU through tools/cmake/PulpGpuWasm.cmake,
    # not through this target).
    target_sources(pulp-gpu-audio PRIVATE
        src/gpu_convolver.cpp src/gpu_multi_convolver.cpp src/gpu_stft.cpp
        src/gpu_spectral_freeze.cpp src/gpu_spectral_morph.cpp
        src/spectral_stack.cpp src/web/web_gpu_convolver.cpp)
    target_link_libraries(pulp-gpu-audio PUBLIC pulp::render pulp::signal)
else()
    target_sources(pulp-gpu-audio PRIVATE src/gpu_convolver_no_render.cpp)
    target_link_libraries(pulp-gpu-audio PUBLIC pulp::signal)
endif()
