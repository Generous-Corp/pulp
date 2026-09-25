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
)

target_compile_features(pulp-gpu-audio PUBLIC cxx_std_20)

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

# These cache values are part of the installed SDK and release-provenance
# contract on every platform. Reset derived provider state before capability
# detection so an Apple D15 build reconfigured with GPU/Skia/Dawn disabled (or
# the same tree configured on another platform) cannot export stale truth.
set(PULP_GPU_AUDIO_VELLUM_D15_PREFIX "" CACHE PATH
    "Installed Vellum D15 prefix for the coordinated GPU-audio provider")
set(PULP_GPU_AUDIO_HAS_VELLUM_D15 FALSE CACHE INTERNAL
    "Pulp GPU-audio is linked through an installed Vellum D15 provider" FORCE)
set(PULP_GPU_AUDIO_VELLUM_RUNTIME_NAME "" CACHE INTERNAL
    "Installed Vellum D15 runtime filename shipped by the Pulp SDK" FORCE)
set(PULP_GPU_AUDIO_VELLUM_NOTICE_DIR "" CACHE INTERNAL
    "Validated Vellum attribution directory shipped by the Pulp SDK" FORCE)
set(PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE FALSE CACHE INTERNAL
    "Whether the Vellum D15 provider has exhaustive builder provenance" FORCE)

if(APPLE AND NOT PULP_IOS AND PULP_HAS_SKIA
   AND EXISTS "${DAWN_LIBRARY}")
    # D15 is deliberately an opt-in installed-prefix integration.  The Vellum
    # GPU dylib owns the linked Dawn definitions and its coordinator owns the
    # one process bootstrap; this Pulp target only consumes that provider.  An
    # absent or incomplete prefix leaves the established P1 provider (and the
    # CPU fallback available to callers) unchanged.
    if(PULP_GPU_AUDIO_VELLUM_D15_PREFIX)
        find_package(Vellum 0.1.7 EXACT CONFIG QUIET
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
            get_target_property(_pulp_vellum_gpu_location Vellum::Gpu
                IMPORTED_LOCATION_RELEASE)
            if(NOT _pulp_vellum_gpu_location)
                get_target_property(_pulp_vellum_gpu_location Vellum::Gpu
                    IMPORTED_LOCATION)
            endif()
            if(NOT _pulp_vellum_gpu_location)
                message(FATAL_ERROR
                    "Pulp GPU-audio: Vellum::Gpu has no installed runtime location")
            endif()
            file(SHA256 "${_pulp_vellum_gpu_location}"
                _pulp_vellum_gpu_sha256)
            if(NOT _pulp_vellum_gpu_sha256 STREQUAL
               "1ab0aafdcfce2e682d8ae65386d93250f2c49d0431561f34a3d7705138d103d3")
                message(FATAL_ERROR
                    "Pulp GPU-audio: Vellum 0.1.7 runtime digest mismatch: "
                    "${_pulp_vellum_gpu_sha256}")
            endif()
            get_filename_component(_pulp_vellum_gpu_runtime_name
                "${_pulp_vellum_gpu_location}" NAME)
            set(PULP_GPU_AUDIO_VELLUM_RUNTIME_NAME
                "${_pulp_vellum_gpu_runtime_name}" CACHE INTERNAL
                "Installed Vellum D15 runtime filename shipped by the Pulp SDK" FORCE)
            set(_pulp_vellum_notice_dir
                "${PULP_GPU_AUDIO_VELLUM_D15_PREFIX}/share/doc/Vellum")
            foreach(_pulp_vellum_notice_file LICENSE.md NOTICE.md DEPENDENCIES.md)
                if(NOT EXISTS
                   "${_pulp_vellum_notice_dir}/${_pulp_vellum_notice_file}")
                    message(FATAL_ERROR
                        "Pulp GPU-audio: the Vellum D15 provider would be "
                        "redistributed without ${_pulp_vellum_notice_file}: "
                        "${_pulp_vellum_notice_dir}")
                endif()
            endforeach()
            set(_pulp_vellum_notice_expected_LICENSE_md
                "35de8f3cebd71bad85e4d9741f3d03cc8deb32349a6568c77c5f4ef11c2911aa")
            set(_pulp_vellum_notice_expected_NOTICE_md
                "6c743360892bf2320833833501e78128860dee8a506f61ea7c944b6bc6ff7493")
            set(_pulp_vellum_notice_expected_DEPENDENCIES_md
                "411ad08d92bcfffd2ef93987bc0aeb983bea9a7ff952319d14cd50d48e4b85d2")
            foreach(_pulp_vellum_notice_file LICENSE.md NOTICE.md DEPENDENCIES.md)
                string(REPLACE "." "_" _pulp_vellum_notice_key
                    "${_pulp_vellum_notice_file}")
                file(SHA256
                    "${_pulp_vellum_notice_dir}/${_pulp_vellum_notice_file}"
                    _pulp_vellum_notice_sha256)
                if(NOT _pulp_vellum_notice_sha256 STREQUAL
                   _pulp_vellum_notice_expected_${_pulp_vellum_notice_key})
                    message(FATAL_ERROR
                        "Pulp GPU-audio: Vellum 0.1.7 "
                        "${_pulp_vellum_notice_file} digest mismatch: "
                        "${_pulp_vellum_notice_sha256}")
                endif()
            endforeach()
            set(PULP_GPU_AUDIO_VELLUM_NOTICE_DIR
                "${_pulp_vellum_notice_dir}" CACHE INTERNAL
                "Validated Vellum attribution directory shipped by the Pulp SDK" FORCE)
            set_property(TARGET Vellum::Gpu PROPERTY
                PULP_RUNTIME_ATTRIBUTION_DIR "${_pulp_vellum_notice_dir}")
            set_property(TARGET Vellum::Gpu PROPERTY
                PULP_RUNTIME_ATTRIBUTION_NAME "Vellum")
            message(STATUS
                "Pulp GPU-audio: Vellum D15 is development-SDK-only until "
                "its renderer builder emits exhaustive transitive provenance")
            pulp_register_macho_linked_runtime_dependency_target(Vellum::Gpu)
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
    target_compile_definitions(pulp-gpu-audio PRIVATE PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO=1)
    if(PULP_GPU_AUDIO_HAS_VELLUM_D15)
        # Do not link Pulp's static Dawn archive on this edge.  The executable
        # keeps undefined Dawn references which resolve through vellum-gpu,
        # avoiding a second process-global proc-table definition.
        target_link_libraries(pulp-gpu-audio PRIVATE
            "$<BUILD_INTERFACE:Vellum::Gpu>"
            "$<BUILD_INTERFACE:Vellum::DawnHeaders>"
            "$<INSTALL_INTERFACE:Pulp::gpu-audio-vellum-provider>")
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
