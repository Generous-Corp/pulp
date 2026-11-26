find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-renderer3d
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer3d_cube_test.cpp)
    target_link_libraries(pulp-test-pulp-vellum-renderer3d PRIVATE
        pulp::vellum-renderer3d Catch2::Catch2WithMain Threads::Threads)
    target_compile_definitions(pulp-test-pulp-vellum-renderer3d PRIVATE
        PULP_VELLUM_RENDERER3D_PNG_DIR="${CMAKE_CURRENT_BINARY_DIR}/renderer3d-png")
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
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-renderer3d
        PROPERTIES LABELS pulp-vellum-renderer3d)
endif()
