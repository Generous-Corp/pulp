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
        src/detail/shared_io_arena.cpp
        src/detail/dawn_submission_tracker.cpp
)

target_compile_features(pulp-gpu-audio PUBLIC cxx_std_20)

if(APPLE AND NOT PULP_IOS AND PULP_HAS_SKIA
   AND EXISTS "${DAWN_LIBRARY}")
    # D15 is deliberately an opt-in installed-prefix integration.  The Vellum
    # GPU dylib owns the linked Dawn definitions and its coordinator owns the
    # one process bootstrap; this Pulp target only consumes that provider.  An
    # absent or incomplete prefix leaves the established P1 provider (and the
    # CPU fallback available to callers) unchanged.
    set(PULP_GPU_AUDIO_VELLUM_D15_PREFIX "" CACHE PATH
        "Installed Vellum D15 prefix for the coordinated GPU-audio provider")
    set(PULP_GPU_AUDIO_HAS_VELLUM_D15 FALSE CACHE INTERNAL
        "Pulp GPU-audio is linked through an installed Vellum D15 provider" FORCE)
    if(PULP_GPU_AUDIO_VELLUM_D15_PREFIX)
        find_package(Vellum 0.1 CONFIG QUIET
            PATHS "${PULP_GPU_AUDIO_VELLUM_D15_PREFIX}"
            PATH_SUFFIXES lib/cmake/Vellum
            NO_DEFAULT_PATH)
        if(TARGET Vellum::Gpu AND TARGET Vellum::DawnHeaders)
            # Imported targets normally stay visible only below this directory.
            # Pulp's exact-provider probes are configured from a sibling
            # directory, so make the two installed-provider entry points
            # visible to that consumer too; their transitive Vellum graph
            # remains owned by the imported package.
            set_property(TARGET Vellum::Gpu PROPERTY IMPORTED_GLOBAL TRUE)
            set_property(TARGET Vellum::DawnHeaders PROPERTY IMPORTED_GLOBAL TRUE)
            set(PULP_GPU_AUDIO_HAS_VELLUM_D15 TRUE CACHE INTERNAL
                "Pulp GPU-audio is linked through an installed Vellum D15 provider" FORCE)
        else()
            message(WARNING
                "Pulp GPU-audio: ${PULP_GPU_AUDIO_VELLUM_D15_PREFIX} does not provide "
                "both Vellum::Gpu and Vellum::DawnHeaders; retaining the staged legacy provider")
        endif()
    endif()

    target_sources(pulp-gpu-audio PRIVATE
        src/detail/dawn_shared_io_provider.cpp)
    if(PULP_GPU_AUDIO_HAS_VELLUM_D15)
        # Do not link Pulp's static Dawn archive on this edge.  The executable
        # keeps undefined Dawn references which resolve through vellum-gpu,
        # avoiding a second process-global proc-table definition.
        target_link_libraries(pulp-gpu-audio PRIVATE
            Vellum::Gpu
            Vellum::DawnHeaders)
        target_compile_definitions(pulp-gpu-audio PRIVATE
            PULP_GPU_AUDIO_HAS_VELLUM_D15=1
            "PULP_GPU_AUDIO_VELLUM_D15_PREFIX=\"${PULP_GPU_AUDIO_VELLUM_D15_PREFIX}\"")
    else()
        target_link_libraries(pulp-gpu-audio PRIVATE skia::skia)
        target_include_directories(pulp-gpu-audio PRIVATE ${SKIA_INCLUDE_DIRS})
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
