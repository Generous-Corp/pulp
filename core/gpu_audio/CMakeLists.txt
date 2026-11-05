add_library(pulp-gpu-audio)
add_library(pulp::gpu-audio ALIAS pulp-gpu-audio)

target_include_directories(pulp-gpu-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # Some public GPU-toolkit headers (gpu_stft.hpp, spectral_stack.hpp, ...)
        # use render::GpuCompute, so they #include <pulp/render/gpu_compute.hpp>. Those
        # render headers are Dawn-free (opaque void* device/queue handles), so
        # exposing render's include dir keeps the gpu_audio public headers
        # self-contained even in a GPU-off configure where pulp::gpu-device isn't
        # linked (the transport is still built there). $<INSTALL_INTERFACE:include>
        # already covers render's headers in the installed SDK.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/render/include>
        $<INSTALL_INTERFACE:include>
)

# The required macOS lane reuses warm CMake trees.  Keep additions/removals in
# this exported public root as configure inputs so the consumption-census facts
# are regenerated when a new SDK header lands, rather than retaining the prior
# header count until an unrelated CMakeLists change occurs.
file(GLOB _pulp_gpu_audio_public_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pulp/gpu_audio/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pulp/gpu_audio/*.hpp")

target_sources(pulp-gpu-audio
    PRIVATE
        src/gpu_audio_transport.cpp
        src/gpu_convolution_reverb.cpp
        src/gpu_wavenet.cpp
        src/gpu_spectral_mask.cpp
        src/gpu_wavenet_realtime_node.cpp
        src/gpu_audio_provider_pool.cpp
        src/gpu_neural_realtime_node.cpp
        src/gpu_neural_model.cpp
        src/detail/wavenet_streaming_backend.cpp
        src/detail/neural_shadow_worker.cpp
        src/detail/realtime_gpu_audio_path.cpp
        src/detail/shared_io_arena.cpp
        src/detail/dawn_submission_tracker.cpp
        src/detail/shared_io_compute_plan.cpp
        src/detail/shared_io_program_session.cpp
        src/detail/shared_io_convolution_executor.cpp
        src/detail/shared_io_transport_bridge.cpp
        src/detail/shared_io_stamped_bridge.cpp
        src/detail/shared_io_convolution_pipeline.cpp
        src/detail/shared_io_convolution_session.cpp
        src/detail/shared_io_trace.cpp
        # Host-memory shared-I/O provider; Dawn-free, built on every platform.
        src/detail/cpu_shared_io_provider.cpp
)

if(APPLE AND NOT PULP_IOS AND PULP_SKIA_PROVIDES_DAWN)
    # The native observer is private qualification tooling; keep Objective-C++
    # and Metal linkage out of non-Apple and iOS builds.
    target_sources(pulp-gpu-audio PRIVATE src/detail/native_runtime_identity_mac.mm)
    target_link_libraries(pulp-gpu-audio PRIVATE "-framework Metal" "-framework IOKit"
                          "-framework CoreFoundation" objc)
else()
    target_sources(pulp-gpu-audio PRIVATE src/detail/native_runtime_identity_stub.cpp)
endif()

target_compile_features(pulp-gpu-audio PUBLIC cxx_std_20)

# Marks the producer capability tuple understood by official SDK provenance.
set(PULP_GPU_AUDIO_CAPABILITY_SCHEMA 1 CACHE INTERNAL
    "GPU-audio installed capability metadata schema" FORCE)

set(PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER OFF CACHE BOOL
    "Enable the experimental shared-I/O realtime path inside GpuConvolver")
if(PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER)
    if(NOT PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
        message(FATAL_ERROR
            "Experimental shared-I/O convolver requires PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON")
    endif()
    target_compile_definitions(pulp-gpu-audio PRIVATE
        PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER=1)
endif()

# The installed SDK and its release provenance still read these values; the
# Vellum D15 provider they described is retired (the Dawn proc table now has
# one owner, gpu-runtime's coordinator), so they are always empty and false.
# Removing them from the SDK provenance schema is a separate change.
set(PULP_GPU_AUDIO_HAS_VELLUM_D15 FALSE CACHE INTERNAL
    "Retired: the Vellum D15 GPU-audio provider" FORCE)
set(PULP_GPU_AUDIO_VELLUM_RUNTIME_NAME "" CACHE INTERNAL
    "Retired: the Vellum D15 runtime filename" FORCE)
set(PULP_GPU_AUDIO_VELLUM_NOTICE_DIR "" CACHE INTERNAL
    "Retired: the Vellum D15 attribution directory" FORCE)
set(PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE FALSE CACHE INTERNAL
    "Retired: the Vellum D15 provider's provenance state" FORCE)
set(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO FALSE CACHE INTERNAL
    "Pulp GPU-audio includes the Dawn shared-I/O provider" FORCE)

if(APPLE AND NOT PULP_IOS AND PULP_SKIA_PROVIDES_DAWN)
    set(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO TRUE CACHE INTERNAL
        "Pulp GPU-audio includes the Dawn shared-I/O provider" FORCE)
    target_sources(pulp-gpu-audio PRIVATE
        src/detail/dawn_shared_io_provider.cpp
        src/detail/dawn_shared_io_wavenet_program.cpp
        src/detail/dawn_wavenet_factory.cpp)
    target_compile_definitions(pulp-gpu-audio PRIVATE PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO=1)
    # The provider's WaveNet program runs the same layer kernel as GpuCompute;
    # the WGSL source lives with the render module (wavenet_layer_wgsl.hpp).
    target_include_directories(pulp-gpu-audio PRIVATE "${CMAKE_SOURCE_DIR}/core/render/src")
    target_link_libraries(pulp-gpu-audio PRIVATE skia::skia)
    target_include_directories(pulp-gpu-audio PRIVATE ${SKIA_INCLUDE_DIRS})
    if(PULP_HAS_GPU_RUNTIME)
        # The shared-I/O provider bootstraps Dawn's proc table through
        # gpu-runtime's coordinator, the image's only installer.
        target_link_libraries(pulp-gpu-audio PRIVATE VellumGpuRuntime::Device)
        target_compile_definitions(pulp-gpu-audio PRIVATE PULP_GPU_AUDIO_HAS_GPU_RUNTIME=1)
    endif()
endif()

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
# fallback). GpuCompute is implemented by the GPU device layer, so the GPU path
# links pulp::gpu-device and never reaches the 2D renderer. Without the device
# layer, compile no-render definitions that satisfy the public API and let
# callers fall back cleanly at runtime.
if(TARGET pulp::gpu-device)
    # src/web/web_gpu_convolver.cpp is the submit/collect (never-blocking)
    # counterpart of the GpuConvolver node, written for a single-threaded event
    # loop. It is plain C++ over the GpuCompute interface, so it builds and is
    # unit-tested natively here even though its reason to exist is the browser
    # worker (which compiles the same TU through tools/cmake/PulpGpuWasm.cmake,
    # not through this target).
    target_sources(pulp-gpu-audio PRIVATE
        src/gpu_convolver.cpp src/gpu_convolver_fallback.cpp src/gpu_multi_convolver.cpp
        src/gpu_stft.cpp
        src/gpu_spectral_freeze.cpp src/gpu_spectral_morph.cpp
        src/spectral_stack.cpp src/web/web_gpu_convolver.cpp)
    target_link_libraries(pulp-gpu-audio PUBLIC pulp::gpu-device pulp::signal)
else()
    target_sources(pulp-gpu-audio PRIVATE src/gpu_convolver_no_render.cpp
        src/gpu_convolver_fallback.cpp)
    target_link_libraries(pulp-gpu-audio PUBLIC pulp::signal)
endif()

# Provider authentication is needed in installed SDK builds even when tests are off.
include("${PROJECT_SOURCE_DIR}/tools/cmake/PulpGpuAudioProviderIdentity.cmake")
pulp_gpu_audio_configure_provider_identity(pulp-gpu-audio)
