# The bridge's tests, added once Catch2 exists (see the root CMakeLists.txt).
# Sources are rooted at PULP_ROOT_DIR, not PROJECT_SOURCE_DIR: when Pulp is
# embedded with add_subdirectory, PROJECT_SOURCE_DIR is the consumer's tree.

find_package(Threads REQUIRED)

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum
        ${PULP_ROOT_DIR}/test/test_pulp_vellum_param_bridge.cpp)
    target_link_libraries(pulp-test-pulp-vellum PRIVATE
        pulp::vellum Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum PROPERTIES LABELS pulp-vellum)

    # The editor tests need Vellum's mirrored view layer; skip them for an SDK
    # without it so the parameter-bridge tests still build.
    if(TARGET Vellum::MirrorView_core)
        add_executable(pulp-test-pulp-vellum-editor
            ${PULP_ROOT_DIR}/test/test_pulp_vellum_editor.cpp)
        target_link_libraries(pulp-test-pulp-vellum-editor PRIVATE
            pulp::vellum Vellum::MirrorView_core Catch2::Catch2WithMain Threads::Threads)
        catch_discover_tests(pulp-test-pulp-vellum-editor PROPERTIES LABELS pulp-vellum-editor)
    else()
        message(STATUS "pulp-vellum: Vellum::MirrorView_core not in this Vellum SDK; "
            "skipping pulp-test-pulp-vellum-editor")
    endif()
endif()
