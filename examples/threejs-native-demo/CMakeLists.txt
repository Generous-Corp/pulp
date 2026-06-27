# Native Three.js bridge demo — real three.webgpu.js on Pulp's Dawn-backed bridge

if(PULP_ENABLE_GPU AND PULP_HAS_THREEJS)
    add_executable(pulp-threejs-native-demo main.cpp)
    target_link_libraries(pulp-threejs-native-demo PRIVATE
        pulp::view
        pulp::render
        pulp::state
    )
    target_compile_definitions(pulp-threejs-native-demo PRIVATE
        PULP_THREEJS_SOURCE_DIR="${threejs_SOURCE_DIR}"
    )

    if(APPLE)
        target_link_options(pulp-threejs-native-demo PRIVATE -Wl,-stack_size,0x2000000)
    elseif(UNIX)
        target_link_options(pulp-threejs-native-demo PRIVATE -Wl,-z,stacksize=16777216)
    endif()

    # Headless `--capture` must not hang at `status: 'starting'`. This
    # test runs the cube demo with --capture and asserts the binary exits
    # within a bounded time (i.e. does NOT hang). When V8 is actually linked
    # and a native Dawn adapter is available, the test additionally asserts
    # that a non-empty PNG was written. On builds without V8 (or without a
    # Dawn adapter), the binary prints an explanatory banner and exits 1 —
    # the test script tolerates that case explicitly so every CI node still
    # exercises the no-hang capture path.
    if(PULP_BUILD_TESTS)
        add_test(
            NAME threejs_native_demo_capture_no_hang
            COMMAND ${CMAKE_COMMAND}
                -DDEMO_BIN=$<TARGET_FILE:pulp-threejs-native-demo>
                -DDEMO_MODE=cube
                -DCAPTURE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-demo-capture-test.png
                -P ${CMAKE_CURRENT_SOURCE_DIR}/capture_test.cmake
        )
        set_tests_properties(threejs_native_demo_capture_no_hang PROPERTIES
            TIMEOUT 30
            LABELS "threejs;gpu;headless"
        )

        add_test(
            NAME threejs_native_demo_gltf_box_capture_no_hang
            COMMAND ${CMAKE_COMMAND}
                -DDEMO_BIN=$<TARGET_FILE:pulp-threejs-native-demo>
                -DDEMO_MODE=gltf-box
                -DCAPTURE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-gltf-box-capture-test.png
                -P ${CMAKE_CURRENT_SOURCE_DIR}/capture_test.cmake
        )
        set_tests_properties(threejs_native_demo_gltf_box_capture_no_hang PROPERTIES
            TIMEOUT 30
            LABELS "threejs;gpu;headless;gltf;texture"
        )

        # Strict (no skip-pass) V8 provider-identity + cube-coexistence gate.
        # Opt in via PULP_VALIDATE_V8_PROVIDER_STRICT so the default Pulp
        # CI lane (which may build without V8) is unaffected. Asserts the sealed
        # v8-builder seal is the linked runtime and that V8 coexists with
        # Dawn/Skia in the real app by writing a non-empty cube PNG.
        if(PULP_VALIDATE_V8_PROVIDER_STRICT)
            set(_pulp_v8_strict_kind "${PULP_V8_PROVIDER_KIND}")
            if(NOT _pulp_v8_strict_kind)
                set(_pulp_v8_strict_kind "v8builder")
            endif()
            add_test(
                NAME v8_provider_identity_strict
                COMMAND ${CMAKE_COMMAND}
                    -DDEMO_BIN=$<TARGET_FILE:pulp-threejs-native-demo>
                    -DCAPTURE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pulp-threejs-provider-identity.png
                    -DEXPECTED_KIND=${_pulp_v8_strict_kind}
                    -DEXPECTED_VERSION=${PULP_V8_EXPECTED_RUNTIME_VERSION}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/provider_identity_test.cmake
            )
            set_tests_properties(v8_provider_identity_strict PROPERTIES
                TIMEOUT 120
                LABELS "threejs;gpu;headless;v8;provider-identity"
            )
            unset(_pulp_v8_strict_kind)
        endif()
    endif()
endif()
