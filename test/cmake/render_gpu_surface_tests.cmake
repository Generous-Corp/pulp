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

    # Direct native Metal availability/timing probe. This is test-only: it
    # establishes the host capability before any production backend integration.
    if(APPLE AND NOT IOS AND NOT PULP_IOS)
        add_executable(pulp-test-native-metal-compute
            test_metal_native_compute.mm)
        target_link_libraries(pulp-test-native-metal-compute PRIVATE
            "-framework Metal" "-framework Foundation")
        add_test(NAME pulp-test-native-metal-compute
            COMMAND pulp-test-native-metal-compute)
        set_tests_properties(pulp-test-native-metal-compute PROPERTIES
            RESOURCE_LOCK pulp_gpu
            SKIP_RETURN_CODE 77
            TIMEOUT 20)
    endif()

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

        target_compile_definitions(pulp-gpu-audio PRIVATE
            PULP_GPU_AUDIO_EXPECTED_DAWN_SHA="${_pulp_gpu_audio_expected_dawn_sha}")

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

            add_executable(pulp-gpu-dawn-shared-io-provider-probe
                test_gpu_dawn_shared_io_provider_probe.cpp)
            target_link_libraries(pulp-gpu-dawn-shared-io-provider-probe PRIVATE
                pulp::gpu-audio)
            target_include_directories(pulp-gpu-dawn-shared-io-provider-probe PRIVATE
                "${PROJECT_SOURCE_DIR}/core/gpu_audio/src")
            if(PULP_GPU_AUDIO_HAS_VELLUM_D15)
                # The probe's test-only proc-table counter includes Dawn
                # declarations, but the definitions remain in vellum-gpu.
                target_link_libraries(pulp-gpu-dawn-shared-io-provider-probe PRIVATE
                    Vellum::Gpu Vellum::DawnHeaders)
            endif()
            target_compile_definitions(pulp-gpu-dawn-shared-io-provider-probe PRIVATE
                PULP_GPU_AUDIO_EXPECTED_DAWN_SHA="${_pulp_gpu_audio_expected_dawn_sha}")
            set(_pulp_gpu_audio_shared_prelink_receipt
                "${_pulp_gpu_audio_identity_dir}/$<CONFIG>/shared-io-pre-link.json")
            set(_pulp_gpu_audio_shared_bound_receipt
                "${_pulp_gpu_audio_identity_dir}/$<CONFIG>/shared-io-bound.json")
            set_property(TARGET pulp-gpu-dawn-shared-io-provider-probe APPEND PROPERTY
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
            add_custom_command(TARGET pulp-gpu-dawn-shared-io-provider-probe PRE_LINK
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
                    --result "${_pulp_gpu_audio_shared_prelink_receipt}"
                VERBATIM)
            add_custom_command(TARGET pulp-gpu-dawn-shared-io-provider-probe POST_BUILD
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                    bind
                    --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                    --platform darwin-arm64
                    --skia-dir "${SKIA_DIR}"
                    --dawn-header "${_pulp_gpu_audio_dawn_header}"
                    --dawn-library "${DAWN_LIBRARY}"
                    --configured-receipt "${_pulp_gpu_audio_configure_receipt}"
                    --prelink-receipt "${_pulp_gpu_audio_shared_prelink_receipt}"
                    --executable "$<TARGET_FILE:pulp-gpu-dawn-shared-io-provider-probe>"
                    --result "${_pulp_gpu_audio_shared_bound_receipt}"
                VERBATIM)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-identity
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/tools/scripts/gpu_audio_provider_identity.py"
                    verify
                    --manifest "${PROJECT_SOURCE_DIR}/tools/deps/manifest.json"
                    --platform darwin-arm64
                    --skia-dir "${SKIA_DIR}"
                    --dawn-header "${_pulp_gpu_audio_dawn_header}"
                    --dawn-library "${DAWN_LIBRARY}"
                    --configured-receipt "${_pulp_gpu_audio_configure_receipt}"
                    --prelink-receipt "${_pulp_gpu_audio_shared_prelink_receipt}"
                    --executable "$<TARGET_FILE:pulp-gpu-dawn-shared-io-provider-probe>"
                    --receipt "${_pulp_gpu_audio_shared_bound_receipt}")
            set_tests_properties(pulp-gpu-dawn-shared-io-provider-identity PROPERTIES
                FIXTURES_REQUIRED pulp_gpu_audio_provider_identity
                FIXTURES_SETUP pulp_gpu_dawn_shared_io_provider_identity
                TIMEOUT 60)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-probe
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-wait-any
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict
                    --completion-policy=wait-any)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-timed-wait-any
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict
                    --completion-policy=timed-wait-any --completion-wait-ns=1000000)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-wait-any-delay
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict
                    --scenario=delay-completion --completion-policy=wait-any)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-wait-any-timeout-recovery
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict
                    --scenario=wait-any-timeout --completion-policy=wait-any)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-wait-any-error-recovery
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict
                    --scenario=wait-any-error --completion-policy=wait-any)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-wait-any-batch
                COMMAND pulp-gpu-dawn-shared-io-provider-probe --strict
                    --scenario=wait-any-batch --completion-policy=wait-any)
            add_test(NAME pulp-gpu-dawn-shared-io-provider-completion-wait-bound
                COMMAND pulp-gpu-dawn-shared-io-provider-probe
                    --verify-completion-wait-bound)
            set_tests_properties(pulp-gpu-dawn-shared-io-provider-probe PROPERTIES
                FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                RESOURCE_LOCK pulp_gpu
                TIMEOUT 20)
            set_tests_properties(
                pulp-gpu-dawn-shared-io-provider-wait-any
                pulp-gpu-dawn-shared-io-provider-timed-wait-any
                pulp-gpu-dawn-shared-io-provider-wait-any-delay
                pulp-gpu-dawn-shared-io-provider-wait-any-timeout-recovery
                pulp-gpu-dawn-shared-io-provider-wait-any-error-recovery
                pulp-gpu-dawn-shared-io-provider-completion-wait-bound
                PROPERTIES
                    FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                    RESOURCE_LOCK pulp_gpu
                    TIMEOUT 20)
            set_tests_properties(pulp-gpu-dawn-shared-io-provider-wait-any-batch PROPERTIES
                FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                RESOURCE_LOCK pulp_gpu
                TIMEOUT 60)

            add_test(NAME pulp-gpu-dawn-shared-io-provider-lifecycle
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/test/verify_gpu_dawn_shared_io_provider.py"
                    --probe "$<TARGET_FILE:pulp-gpu-dawn-shared-io-provider-probe>")
            set_tests_properties(pulp-gpu-dawn-shared-io-provider-lifecycle PROPERTIES
                FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                RESOURCE_LOCK pulp_gpu
                TIMEOUT 60)

            # D15 is intentionally consumed through Vellum's installed CMake
            # package.  This exercises the coordinator's failure, re-entry,
            # and idempotence contract in the same linkage shape Pulp uses,
            # then proves the final executable did not acquire a second Dawn
            # definition owner from Pulp's static dependency graph.
            if(PULP_GPU_AUDIO_HAS_VELLUM_D15)
                add_executable(pulp-test-gpu-dawn-vellum-bootstrap-contract
                    test_gpu_dawn_vellum_bootstrap_contract.cpp)
                target_link_libraries(pulp-test-gpu-dawn-vellum-bootstrap-contract PRIVATE
                    Vellum::Gpu)
                target_compile_definitions(pulp-test-gpu-dawn-vellum-bootstrap-contract PRIVATE
                    "PULP_GPU_AUDIO_VELLUM_D15_PREFIX=\"${PULP_GPU_AUDIO_VELLUM_D15_PREFIX}\"")
                add_test(NAME pulp-gpu-dawn-vellum-bootstrap-contract
                    COMMAND pulp-test-gpu-dawn-vellum-bootstrap-contract)
                set_tests_properties(pulp-gpu-dawn-vellum-bootstrap-contract PROPERTIES
                    TIMEOUT 20)

                add_test(NAME pulp-gpu-dawn-vellum-provider-topology
                    COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/test/verify_gpu_dawn_vellum_provider_topology.py"
                        "$<TARGET_FILE:pulp-gpu-dawn-shared-io-provider-probe>"
                        "$<TARGET_FILE:Vellum::Gpu>")
                set_tests_properties(pulp-gpu-dawn-vellum-provider-topology PROPERTIES
                    FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                    TIMEOUT 20)
            endif()

            add_test(NAME pulp-gpu-dawn-vellum-d15-source
                COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/test/test_gpu_dawn_vellum_d15_source.py")
            set_tests_properties(pulp-gpu-dawn-vellum-d15-source PROPERTIES
                TIMEOUT 20)

            add_executable(pulp-gpu-shared-io-private-convolution-probe
                test_gpu_shared_io_private_convolution_probe.cpp)
            target_link_libraries(pulp-gpu-shared-io-private-convolution-probe PRIVATE
                pulp::gpu-audio)
            target_include_directories(pulp-gpu-shared-io-private-convolution-probe PRIVATE
                ../core/gpu_audio/src)
            add_dependencies(pulp-gpu-shared-io-private-convolution-probe
                pulp-gpu-dawn-shared-io-provider-probe)
            add_test(NAME pulp-gpu-shared-io-private-convolution-probe
                COMMAND pulp-gpu-shared-io-private-convolution-probe)
            add_test(NAME pulp-gpu-shared-io-private-convolution-prepare-scope-negative-control
                COMMAND pulp-gpu-shared-io-private-convolution-probe
                    --scenario=prepare-scope-failure)
            add_test(NAME pulp-gpu-shared-io-private-convolution-submit-scope-negative-control
                COMMAND pulp-gpu-shared-io-private-convolution-probe
                    --scenario=submit-scope-failure)
            set_tests_properties(pulp-gpu-shared-io-private-convolution-probe PROPERTIES
                FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                RESOURCE_LOCK pulp_gpu
                TIMEOUT 60)
            set_tests_properties(
                pulp-gpu-shared-io-private-convolution-prepare-scope-negative-control
                pulp-gpu-shared-io-private-convolution-submit-scope-negative-control
                PROPERTIES
                    FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                    RESOURCE_LOCK pulp_gpu
                    TIMEOUT 60)

            if(PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER)
                add_executable(pulp-gpu-shared-io-paced-convolution-probe
                    test_gpu_shared_io_paced_convolution_probe.cpp)
                target_link_libraries(pulp-gpu-shared-io-paced-convolution-probe PRIVATE
                    pulp::gpu-audio)
                target_include_directories(pulp-gpu-shared-io-paced-convolution-probe PRIVATE
                    ../core/gpu_audio/src)
                add_dependencies(pulp-gpu-shared-io-paced-convolution-probe
                    pulp-gpu-dawn-shared-io-provider-probe)
                add_test(NAME pulp-gpu-shared-io-paced-convolution-probe
                    COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/test/verify_gpu_shared_io_paced_convolution.py"
                        --probe "$<TARGET_FILE:pulp-gpu-shared-io-paced-convolution-probe>")
                set_tests_properties(pulp-gpu-shared-io-paced-convolution-probe PROPERTIES
                    FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                    RESOURCE_LOCK pulp_gpu
                    TIMEOUT 150)
            endif()

            # This goes through the private session factory rather than
            # driving the plan and executor separately: one real provider
            # creates its paired program, the session owns both, and callback
            # ingress/egress stays free of GPU transfers.
            add_executable(pulp-gpu-shared-io-convolution-session-probe
                test_gpu_shared_io_convolution_session_probe.cpp)
            target_link_libraries(pulp-gpu-shared-io-convolution-session-probe PRIVATE
                pulp::gpu-audio)
            target_include_directories(pulp-gpu-shared-io-convolution-session-probe PRIVATE
                ../core/gpu_audio/src)
            target_compile_definitions(pulp-gpu-shared-io-convolution-session-probe PRIVATE
                PULP_GPU_AUDIO_EXPECTED_DAWN_SHA="${_pulp_gpu_audio_expected_dawn_sha}")
            if(PULP_GPU_AUDIO_HAS_VELLUM_D15)
                target_link_libraries(pulp-gpu-shared-io-convolution-session-probe PRIVATE
                    Vellum::Gpu Vellum::DawnHeaders)
            endif()
            add_dependencies(pulp-gpu-shared-io-convolution-session-probe
                pulp-gpu-dawn-shared-io-provider-probe)
            add_test(NAME pulp-gpu-shared-io-convolution-session-probe
                COMMAND pulp-gpu-shared-io-convolution-session-probe)
            add_test(NAME pulp-gpu-shared-io-convolution-session-prepare-scope-negative-control
                COMMAND pulp-gpu-shared-io-convolution-session-probe
                    --scenario=prepare-scope-failure)
            add_test(NAME pulp-gpu-shared-io-convolution-session-submit-scope-negative-control
                COMMAND pulp-gpu-shared-io-convolution-session-probe
                    --scenario=submit-scope-failure)
            set_tests_properties(
                pulp-gpu-shared-io-convolution-session-probe
                pulp-gpu-shared-io-convolution-session-prepare-scope-negative-control
                pulp-gpu-shared-io-convolution-session-submit-scope-negative-control
                PROPERTIES
                    FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                    RESOURCE_LOCK pulp_gpu
                    TIMEOUT 60)

            if(PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER)
                # Matched staged/shared lifecycle screening. This is deliberately
                # an intermediate p4.matched.v1 diagnostic and never emits p4.raw.v1:
                # callback/result-visible timing and transfer provenance remain
                # explicit unavailable fields until the strict campaign seam lands.
                add_executable(pulp-gpu-audio-p4-matched-convolution-benchmark
                    test_gpu_audio_p4_matched_convolution_benchmark.cpp)
                target_link_libraries(pulp-gpu-audio-p4-matched-convolution-benchmark PRIVATE
                    pulp::gpu-audio)
                target_include_directories(pulp-gpu-audio-p4-matched-convolution-benchmark PRIVATE
                    ../core/gpu_audio/src)
                add_dependencies(pulp-gpu-audio-p4-matched-convolution-benchmark
                    pulp-gpu-dawn-shared-io-provider-probe)
                add_test(NAME pulp-gpu-audio-p4-matched-convolution-benchmark
                    COMMAND pulp-gpu-audio-p4-matched-convolution-benchmark)
                set_tests_properties(pulp-gpu-audio-p4-matched-convolution-benchmark PROPERTIES
                    FIXTURES_REQUIRED pulp_gpu_dawn_shared_io_provider_identity
                    RESOURCE_LOCK pulp_gpu
                    SKIP_RETURN_CODE 77
                    TIMEOUT 120)
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
