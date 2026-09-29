# Production GPU-audio provider identity. Tests consume these properties; they
# never initialize the library identity. Exact proof validates pinned bytes both
# at configure and before every library build, including SDK-only builds.
function(pulp_gpu_audio_configure_provider_identity target)
    set(_pulp_gpu_audio_target_archs "${CMAKE_OSX_ARCHITECTURES}")
    if(APPLE AND NOT _pulp_gpu_audio_target_archs)
        set(_pulp_gpu_audio_target_archs "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    list(LENGTH _pulp_gpu_audio_target_archs _pulp_gpu_audio_target_arch_count)
    set(_pulp_gpu_audio_target_is_arm64 FALSE)
    if(_pulp_gpu_audio_target_arch_count EQUAL 1)
        list(GET _pulp_gpu_audio_target_archs 0 _pulp_gpu_audio_target_arch)
        if(_pulp_gpu_audio_target_arch MATCHES "^(arm64|aarch64)$")
            set(_pulp_gpu_audio_target_is_arm64 TRUE)
        endif()
    endif()
    if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF AND NOT
            (APPLE AND NOT IOS AND NOT PULP_IOS
             AND _pulp_gpu_audio_target_is_arm64))
        message(FATAL_ERROR
            "PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF requires a thin Apple-Silicon macOS target")
    endif()
    if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF AND
            (NOT PULP_HAS_SKIA OR NOT EXISTS "${DAWN_LIBRARY}"))
        message(FATAL_ERROR
            "PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF requires the pinned Skia/Dawn provider")
    endif()

    if(NOT (APPLE AND NOT IOS AND NOT PULP_IOS AND PULP_HAS_SKIA
            AND EXISTS "${DAWN_LIBRARY}"))
        return()
    endif()
    if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    file(SHA256 "${DAWN_LIBRARY}" _pulp_gpu_audio_dawn_archive_sha256)
    set(_pulp_gpu_audio_asset_sha256 "unknown")
    if(EXISTS "${SKIA_DIR}/.skia-asset-sha256")
        file(READ "${SKIA_DIR}/.skia-asset-sha256"
            _pulp_gpu_audio_asset_sha256)
        string(STRIP "${_pulp_gpu_audio_asset_sha256}"
            _pulp_gpu_audio_asset_sha256)
    endif()

    # Non-exact builds deliberately carry no expected Dawn revision. A
    # concrete revision is emitted only after the manifest-bound exact-provider
    # comparison succeeds.
    set(_pulp_gpu_audio_expected_dawn_sha "")
    if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
        list(GET SKIA_INCLUDE_DIRS 0 _pulp_gpu_audio_skia_include_root)
        set(_pulp_gpu_audio_dawn_header
            "${_pulp_gpu_audio_skia_include_root}/dawn/dawn_version.h")
        set(_pulp_gpu_audio_identity_dir
            "${CMAKE_BINARY_DIR}/gpu-audio-provider-identity")
        set(_pulp_gpu_audio_identity_cmake
            "${_pulp_gpu_audio_identity_dir}/identity.cmake")
        set(_pulp_gpu_audio_configure_receipt
            "${_pulp_gpu_audio_identity_dir}/configure.json")
        set(_pulp_gpu_audio_prelink_receipt
            "${_pulp_gpu_audio_identity_dir}/$<CONFIG>/pre-link.json")
        set(_pulp_gpu_audio_bound_receipt
            "${_pulp_gpu_audio_identity_dir}/$<CONFIG>/bound.json")
        file(MAKE_DIRECTORY "${_pulp_gpu_audio_identity_dir}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                validate
                --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                --platform darwin-arm64
                --skia-dir "${SKIA_DIR}"
                --dawn-header "${_pulp_gpu_audio_dawn_header}"
                --dawn-library "${DAWN_LIBRARY}"
                --result "${_pulp_gpu_audio_configure_receipt}"
                --cmake-output "${_pulp_gpu_audio_identity_cmake}"
            RESULT_VARIABLE _pulp_gpu_audio_identity_result
            OUTPUT_VARIABLE _pulp_gpu_audio_identity_output
            ERROR_VARIABLE _pulp_gpu_audio_identity_error)
        if(NOT _pulp_gpu_audio_identity_result EQUAL 0)
            message(FATAL_ERROR
                "GPU-audio exact-provider validation failed:\n"
                "${_pulp_gpu_audio_identity_output}"
                "${_pulp_gpu_audio_identity_error}")
        endif()
        include("${_pulp_gpu_audio_identity_cmake}")
        set(_pulp_gpu_audio_expected_dawn_sha
            "${PULP_GPU_AUDIO_EXPECTED_DAWN_SHA}")
        set(_pulp_gpu_audio_asset_sha256
            "${PULP_GPU_AUDIO_EXPECTED_ASSET_SHA256}")
        set(_pulp_gpu_audio_dawn_archive_sha256
            "${PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256}")
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_GPU_AUDIO_EXPECTED_DAWN_SHA="${_pulp_gpu_audio_expected_dawn_sha}")
    foreach(key expected_dawn_sha asset_sha256 dawn_archive_sha256 dawn_header
                identity_dir configure_receipt prelink_receipt bound_receipt)
        set_property(TARGET ${target} PROPERTY "PULP_PROVIDER_${key}"
            "${_pulp_gpu_audio_${key}}")
    endforeach()
    if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
        add_custom_target(pulp-gpu-audio-provider-prelink
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${_pulp_gpu_audio_identity_dir}/$<CONFIG>"
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                prelink
                --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                --platform darwin-arm64 --skia-dir "${SKIA_DIR}"
                --dawn-header "${_pulp_gpu_audio_dawn_header}"
                --dawn-library "${DAWN_LIBRARY}"
                --configured-receipt "${_pulp_gpu_audio_configure_receipt}"
                --result "${_pulp_gpu_audio_identity_dir}/$<CONFIG>/library-pre-link.json"
            VERBATIM)
        add_dependencies(${target} pulp-gpu-audio-provider-prelink)
    endif()
endfunction()
