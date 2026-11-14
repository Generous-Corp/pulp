# The bridge's tests, added once Catch2 exists (see the root CMakeLists.txt).
# Sources are rooted at PULP_ROOT_DIR, not PROJECT_SOURCE_DIR: when Pulp is
# embedded with add_subdirectory, PROJECT_SOURCE_DIR is the consumer's tree.

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    find_package(Threads REQUIRED)
    add_executable(pulp-test-pulp-vellum
        ${PULP_ROOT_DIR}/test/test_pulp_vellum_param_bridge.cpp)
    target_link_libraries(pulp-test-pulp-vellum PRIVATE
        pulp::vellum Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum PROPERTIES LABELS pulp-vellum)
endif()
