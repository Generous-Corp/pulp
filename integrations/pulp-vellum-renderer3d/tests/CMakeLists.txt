find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-renderer3d
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_cube_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_materials_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_scene_test.cpp)
    # The CPU module tests include the package's private headers.
    target_include_directories(pulp-test-pulp-vellum-renderer3d PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src ${SKIA_INCLUDE_DIRS} "${SKIA_INCLUDE_DIRS}/dawn")
    target_link_libraries(pulp-test-pulp-vellum-renderer3d PRIVATE
        pulp::vellum-renderer3d Catch2::Catch2WithMain Threads::Threads)
    target_compile_definitions(pulp-test-pulp-vellum-renderer3d PRIVATE
        PULP_VELLUM_RENDERER3D_PNG_DIR="${CMAKE_CURRENT_BINARY_DIR}/renderer3d-png"
        PULP_VELLUM_RENDERER3D_FIXTURE_DIR="${PULP_ROOT_DIR}/test/fixtures/scene3d")
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
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-renderer3d
        PROPERTIES LABELS pulp-vellum-renderer3d)
endif()
