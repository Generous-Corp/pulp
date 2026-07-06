add_library(pulp-gpu-audio)
add_library(pulp::gpu-audio ALIAS pulp-gpu-audio)

target_include_directories(pulp-gpu-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # The public GPU-node headers (gpu_convolver.hpp, gpu_stft.hpp, …) name
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
# primitives and the lock-free runtime. Later slices add pulp::signal (CPU
# fallbacks like PartitionedConvolver) and pulp::render (GPU nodes) when the
# code actually uses them.
find_package(Threads REQUIRED)
target_link_libraries(pulp-gpu-audio
    PUBLIC
        pulp::audio
        pulp::runtime
    PRIVATE
        Threads::Threads
)

# The GPU convolver node uses render::GpuCompute (FFT) + signal::Convolver (CPU
# fallback). Built only when GPU is enabled (pulp::render exists); the transport
# above stays GPU-agnostic. The GPU path is a runtime no-op without a Skia
# device (GpuCompute::create() returns null → CPU fallback).
if(TARGET pulp::render)
    target_sources(pulp-gpu-audio PRIVATE
        src/gpu_convolver.cpp src/gpu_multi_convolver.cpp src/gpu_stft.cpp
        src/gpu_spectral_freeze.cpp src/gpu_spectral_morph.cpp
        src/gpu_hyper_freeze.cpp src/spectral_stack.cpp)
    target_link_libraries(pulp-gpu-audio PUBLIC pulp::render pulp::signal)
endif()
