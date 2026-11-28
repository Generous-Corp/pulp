find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-renderer3d
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_cube_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_materials_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_scene_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/suite_loading_test.cpp)
    # The CPU module tests include the package's private headers.
    target_include_directories(pulp-test-pulp-vellum-renderer3d PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src ${SKIA_INCLUDE_DIRS} "${SKIA_INCLUDE_DIRS}/dawn")
    target_link_libraries(pulp-test-pulp-vellum-renderer3d PRIVATE
        pulp::vellum-renderer3d Catch2::Catch2WithMain Threads::Threads)
    target_compile_definitions(pulp-test-pulp-vellum-renderer3d PRIVATE
        PULP_VELLUM_RENDERER3D_PNG_DIR="${CMAKE_CURRENT_BINARY_DIR}/renderer3d-png"
        PULP_VELLUM_RENDERER3D_FIXTURE_DIR="${PULP_ROOT_DIR}/test/fixtures/scene3d"
        PULP_TEST_SCENE3D_FIXTURE_DIR="${PULP_ROOT_DIR}/test/fixtures/scene3d")
    # While core/render still carries the legacy renderer, the parity case runs
    # its probe out of process (no legacy header in this binary), decodes the
    # PNG it writes and compares it with the ported frame pixel for pixel.
    if(TARGET pulp-renderer3d-probe)
        add_dependencies(pulp-test-pulp-vellum-renderer3d pulp-renderer3d-probe)
        target_link_libraries(pulp-test-pulp-vellum-renderer3d PRIVATE skia::skia)
        target_include_directories(pulp-test-pulp-vellum-renderer3d PRIVATE ${SKIA_INCLUDE_DIRS})
        target_compile_definitions(pulp-test-pulp-vellum-renderer3d PRIVATE
            PULP_VELLUM_RENDERER3D_LEGACY_PROBE="$<TARGET_FILE:pulp-renderer3d-probe>")
    endif()
    # While core/render keeps its copy of the scene shader, the two must match.
    if(EXISTS "${PULP_ROOT_DIR}/core/render/shaders/renderer3d_scene.wgsl")
        add_test(NAME pulp-vellum-renderer3d-wgsl-twin
            COMMAND ${CMAKE_COMMAND}
                -DPORTED=${CMAKE_CURRENT_SOURCE_DIR}/../shaders/renderer3d_scene.wgsl
                -DLEGACY=${PULP_ROOT_DIR}/core/render/shaders/renderer3d_scene.wgsl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_wgsl.cmake)
        set_tests_properties(pulp-vellum-renderer3d-wgsl-twin PROPERTIES
            LABELS pulp-vellum-renderer3d)
    endif()
    # The probe against the macOS default Metal adapter's recorded frames: the
    # same fingerprints the legacy probe tests and renderer3d-goldens.json
    # record. Another adapter draws other bytes, so off Apple those runs skip.
    set(_r3d_fixture "${PULP_ROOT_DIR}/test/fixtures/scene3d/BoxTextured/BoxTextured.glb")
    set(_r3d_scenes hardcoded boxtextured)
    set(_r3d_hardcoded_args --scene hardcoded)
    set(_r3d_hardcoded_fingerprint 15925498132503966243)
    set(_r3d_hardcoded_entry hardcoded_textured_cube)
    set(_r3d_boxtextured_args --scene boxtextured --fixture "${_r3d_fixture}")
    set(_r3d_boxtextured_fingerprint 5845745157752120258)
    set(_r3d_boxtextured_entry official_boxtextured_fixture)
    find_package(Python3 COMPONENTS Interpreter)
    foreach(_scene IN LISTS _r3d_scenes)
        set(_args ${_r3d_${_scene}_args} --width 128 --height 128
            --adapter-scope macos_default_metal)
        add_test(NAME pulp-vellum-renderer3d-probe-${_scene}
            COMMAND $<TARGET_FILE:pulp-vellum-renderer3d-probe> ${_args})
        set_tests_properties(pulp-vellum-renderer3d-probe-${_scene} PROPERTIES
            LABELS pulp-vellum-renderer3d
            PASS_REGULAR_EXPRESSION
                "scene=${_scene}.*success=true.*readback_completed=true.*rgba_fingerprint=${_r3d_${_scene}_fingerprint}.*final_software_golden_eligible=false")
        if(NOT APPLE)
            set_tests_properties(pulp-vellum-renderer3d-probe-${_scene} PROPERTIES
                SKIP_REGULAR_EXPRESSION
                    "adapter_backend_type=(Null|WebGPU|D3D11|D3D12|Vulkan|OpenGL)")
        endif()
        if(Python3_Interpreter_FOUND)
            add_test(NAME pulp-vellum-renderer3d-probe-${_scene}-manifest
                COMMAND ${Python3_EXECUTABLE}
                    "${PULP_ROOT_DIR}/tools/scene3d/verify_renderer_probe.py"
                    --manifest "${PULP_ROOT_DIR}/test/fixtures/scene3d/renderer3d-goldens.json"
                    --entry-id ${_r3d_${_scene}_entry}
                    --
                    $<TARGET_FILE:pulp-vellum-renderer3d-probe> ${_args})
            set_tests_properties(pulp-vellum-renderer3d-probe-${_scene}-manifest PROPERTIES
                LABELS pulp-vellum-renderer3d
                PASS_REGULAR_EXPRESSION "renderer_probe_verified=${_r3d_${_scene}_entry}"
                SKIP_REGULAR_EXPRESSION "renderer_probe_golden_not_applicable=")
        endif()
    endforeach()

    include(Catch)
    # The suite_* files are copies of core's test_renderer3d_* suites, so the
    # prefix keeps their ctest names apart from the legacy ones.
    catch_discover_tests(pulp-test-pulp-vellum-renderer3d
        TEST_PREFIX "vellum-renderer3d: "
        PROPERTIES LABELS pulp-vellum-renderer3d)
endif()
