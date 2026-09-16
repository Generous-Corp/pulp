# GPU surface, Skia context, headless renderer, and compute smoke tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

    add_executable(pulp-test-gpu-surface test_gpu_surface.cpp)
    target_link_libraries(pulp-test-gpu-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # Skia surface test — also covers SkPicture (.skp) serialization on the
    # Graphite backend. The .skp round-trip cases include
    # Skia headers directly (SkPicture, SkPictureRecorder, SkSerialProcs,
    # SkPngEncoder), so this target needs SKIA_INCLUDE_DIRS even though
    # pulp::render carries Skia privately.
    # pulp::view for the SvgPathWidget gradient-stroke raster proof.
    add_executable(pulp-test-skia-surface test_skia_surface.cpp)
    target_link_libraries(pulp-test-skia-surface PRIVATE pulp::render pulp::view Catch2::Catch2WithMain)
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-skia-surface PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-skia-surface PRIVATE ${SKIA_INCLUDE_DIRS})
        if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
            target_include_directories(pulp-test-skia-surface PRIVATE
                "${SKIA_INCLUDE_DIRS}/dawn")
        endif()
    endif()
    catch_discover_tests(pulp-test-skia-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # HeadlessSurface CI wrapper: one-call offscreen Dawn/Skia helper
    # for golden-test fixtures. Compiles on every
    # build; runtime cases gate on PULP_HAS_SKIA && __APPLE__ at the
    # source level and otherwise soft-skip when no Dawn adapter is
    # available. Needs Skia headers directly for the SkPngEncoder /
    # SkPixmap symbols the wrapper uses to PNG-encode the readback.
    add_executable(pulp-test-headless-surface test_headless_surface.cpp)
    target_link_libraries(pulp-test-headless-surface PRIVATE
        pulp::render pulp::canvas Catch2::Catch2WithMain)
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-headless-surface PRIVATE
            PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-headless-surface PRIVATE
            ${SKIA_INCLUDE_DIRS})
    endif()
    catch_discover_tests(pulp-test-headless-surface
        ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # GPU compute tests.
    add_executable(pulp-test-gpu-compute test_gpu_compute.cpp)
    target_link_libraries(pulp-test-gpu-compute PRIVATE pulp::render pulp::signal Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-compute ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # Native Apple-Silicon capability proof for Dawn's HostMappedPointer path.
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

    # This is a standalone probe rather than a Catch test because CI and field
    # machines need distinct pass / failed / unavailable exit states plus one
    # bounded JSON receipt with the exact provider identity.
    if(APPLE AND NOT IOS AND NOT PULP_IOS AND PULP_HAS_SKIA
            AND EXISTS "${DAWN_LIBRARY}")
        file(SHA256 "${DAWN_LIBRARY}" _pulp_gpu_audio_dawn_archive_sha256)
        set(_pulp_gpu_audio_asset_sha256 "unknown")
        if(EXISTS "${SKIA_DIR}/.skia-asset-sha256")
            file(READ "${SKIA_DIR}/.skia-asset-sha256"
                _pulp_gpu_audio_asset_sha256)
            string(STRIP "${_pulp_gpu_audio_asset_sha256}"
                _pulp_gpu_audio_asset_sha256)
        endif()

        set(_pulp_gpu_audio_expected_dawn_sha "unknown")
        if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
            list(GET SKIA_INCLUDE_DIRS 0 _pulp_gpu_audio_skia_include_root)
            set(_pulp_gpu_audio_dawn_header
                "${_pulp_gpu_audio_skia_include_root}/dawn/dawn_version.h")
            set(_pulp_gpu_audio_identity_dir
                "${CMAKE_CURRENT_BINARY_DIR}/gpu-audio-provider-identity")
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

        add_executable(pulp-gpu-host-mapped-pointer-probe
            test_gpu_host_mapped_pointer_probe.cpp)
        target_link_libraries(pulp-gpu-host-mapped-pointer-probe PRIVATE
            "${DAWN_LIBRARY}"
            "-framework Metal"
            "-framework Foundation"
            "-framework IOKit"
            "-framework IOSurface"
            "-framework QuartzCore"
            objc)
        target_include_directories(pulp-gpu-host-mapped-pointer-probe PRIVATE
            ${SKIA_INCLUDE_DIRS})
        if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
            target_include_directories(pulp-gpu-host-mapped-pointer-probe PRIVATE
                "${SKIA_INCLUDE_DIRS}/dawn")
        endif()
        target_compile_definitions(pulp-gpu-host-mapped-pointer-probe PRIVATE
            PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256="${_pulp_gpu_audio_asset_sha256}"
            PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256="${_pulp_gpu_audio_dawn_archive_sha256}"
            PULP_GPU_AUDIO_EXPECTED_DAWN_SHA="${_pulp_gpu_audio_expected_dawn_sha}"
            PULP_GPU_AUDIO_BUILD_TYPE="$<CONFIG>")
        if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
            set_property(TARGET pulp-gpu-host-mapped-pointer-probe APPEND PROPERTY
                LINK_DEPENDS
                    "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/fetch_skia_for_release.py"
                    "${_pulp_gpu_audio_configure_receipt}"
                    "${_pulp_gpu_audio_dawn_header}"
                    "${DAWN_LIBRARY}"
                    "${SKIA_DIR}/.skia-asset-sha256"
                    "${SKIA_DIR}/.skia-generation-manifest.json"
                    "${SKIA_DIR}/.skia-source-archive.zip")
            add_custom_command(TARGET pulp-gpu-host-mapped-pointer-probe PRE_LINK
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_pulp_gpu_audio_identity_dir}/$<CONFIG>"
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                    prelink
                    --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                    --platform darwin-arm64
                    --skia-dir "${SKIA_DIR}"
                    --dawn-header "${_pulp_gpu_audio_dawn_header}"
                    --dawn-library "${DAWN_LIBRARY}"
                    --configured-receipt "${_pulp_gpu_audio_configure_receipt}"
                    --result "${_pulp_gpu_audio_prelink_receipt}"
                VERBATIM)
            add_custom_command(TARGET pulp-gpu-host-mapped-pointer-probe POST_BUILD
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                    bind
                    --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                    --platform darwin-arm64
                    --skia-dir "${SKIA_DIR}"
                    --dawn-header "${_pulp_gpu_audio_dawn_header}"
                    --dawn-library "${DAWN_LIBRARY}"
                    --configured-receipt "${_pulp_gpu_audio_configure_receipt}"
                    --prelink-receipt "${_pulp_gpu_audio_prelink_receipt}"
                    --executable "$<TARGET_FILE:pulp-gpu-host-mapped-pointer-probe>"
                    --result "${_pulp_gpu_audio_bound_receipt}"
                VERBATIM)
            add_test(NAME pulp-gpu-audio-provider-identity
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                    verify
                    --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                    --platform darwin-arm64
                    --skia-dir "${SKIA_DIR}"
                    --dawn-header "${_pulp_gpu_audio_dawn_header}"
                    --dawn-library "${DAWN_LIBRARY}"
                    --configured-receipt "${_pulp_gpu_audio_configure_receipt}"
                    --prelink-receipt "${_pulp_gpu_audio_prelink_receipt}"
                    --executable "$<TARGET_FILE:pulp-gpu-host-mapped-pointer-probe>"
                    --receipt "${_pulp_gpu_audio_bound_receipt}")
            set_tests_properties(pulp-gpu-audio-provider-identity PROPERTIES
                FIXTURES_SETUP pulp_gpu_audio_provider_identity
                TIMEOUT 60)
        endif()
        add_test(NAME pulp-gpu-host-mapped-pointer-probe
            COMMAND pulp-gpu-host-mapped-pointer-probe)
        add_test(NAME pulp-gpu-host-mapped-pointer-oracle-negative-control
            COMMAND pulp-gpu-host-mapped-pointer-probe
                --verify-oracle-negative-control)
        add_test(NAME pulp-gpu-host-mapped-pointer-feature-gate-negative-control
            COMMAND pulp-gpu-host-mapped-pointer-probe
                --verify-feature-gate-negative-control)
        add_test(NAME pulp-gpu-host-mapped-pointer-write-buffer-counter-negative-control
            COMMAND pulp-gpu-host-mapped-pointer-probe
                --verify-write-buffer-counter-negative-control)
        add_test(NAME pulp-gpu-host-mapped-pointer-copy-buffer-counter-negative-control
            COMMAND pulp-gpu-host-mapped-pointer-probe
                --verify-copy-buffer-counter-negative-control)
        add_test(NAME pulp-gpu-host-mapped-pointer-map-async-counter-negative-control
            COMMAND pulp-gpu-host-mapped-pointer-probe
                --verify-map-async-counter-negative-control)
        if(_pulp_gpu_audio_target_is_arm64)
            add_test(NAME pulp-gpu-host-mapped-pointer-provider-negative-control
                COMMAND "${CMAKE_COMMAND}"
                    "-DPROBE=$<TARGET_FILE:pulp-gpu-host-mapped-pointer-probe>"
                    -P "${CMAKE_SOURCE_DIR}/test/cmake/verify_gpu_audio_provider_mismatch.cmake")
            set_tests_properties(
                pulp-gpu-host-mapped-pointer-provider-negative-control
                PROPERTIES
                    RESOURCE_LOCK pulp_gpu
                    TIMEOUT 20)
        endif()
        set_tests_properties(
            pulp-gpu-host-mapped-pointer-probe
            pulp-gpu-host-mapped-pointer-oracle-negative-control
            pulp-gpu-host-mapped-pointer-feature-gate-negative-control
            pulp-gpu-host-mapped-pointer-write-buffer-counter-negative-control
            pulp-gpu-host-mapped-pointer-copy-buffer-counter-negative-control
            pulp-gpu-host-mapped-pointer-map-async-counter-negative-control
            PROPERTIES
                RESOURCE_LOCK pulp_gpu
                SKIP_RETURN_CODE 77
                TIMEOUT 20)
        if(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
            set_tests_properties(
                pulp-gpu-host-mapped-pointer-probe
                pulp-gpu-host-mapped-pointer-oracle-negative-control
                pulp-gpu-host-mapped-pointer-feature-gate-negative-control
                pulp-gpu-host-mapped-pointer-write-buffer-counter-negative-control
                pulp-gpu-host-mapped-pointer-copy-buffer-counter-negative-control
                pulp-gpu-host-mapped-pointer-map-async-counter-negative-control
                PROPERTIES FIXTURES_REQUIRED pulp_gpu_audio_provider_identity)
            if(_pulp_gpu_audio_target_is_arm64)
                set_tests_properties(
                    pulp-gpu-host-mapped-pointer-provider-negative-control
                    PROPERTIES FIXTURES_REQUIRED pulp_gpu_audio_provider_identity)
            endif()
        endif()

        unset(_pulp_gpu_audio_asset_sha256)
        unset(_pulp_gpu_audio_dawn_archive_sha256)
        unset(_pulp_gpu_audio_expected_dawn_sha)
    endif()
    unset(_pulp_gpu_audio_target_arch)
    unset(_pulp_gpu_audio_target_arch_count)
    unset(_pulp_gpu_audio_target_archs)
    unset(_pulp_gpu_audio_target_is_arm64)

    # GPU roofline / occupancy harness (tooling, not a test): drives every
    # MAC-dense compute pass and prints a ranked GMAC/s-vs-roofline + occupancy
    # table. Confirms the WaveNet one-thread-per-sample gap and its siblings.
    add_executable(pulp-gpu-roofline-harness
        "${PROJECT_SOURCE_DIR}/tools/gpu_roofline/gpu_roofline_harness.cpp")
    target_link_libraries(pulp-gpu-roofline-harness PRIVATE pulp::render pulp::signal)
