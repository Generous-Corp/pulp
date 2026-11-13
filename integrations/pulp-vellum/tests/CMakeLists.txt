# The bridge's tests, added once Catch2 exists (see the root CMakeLists.txt).

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    find_package(Threads REQUIRED)
    add_executable(pulp-test-pulp-vellum
        ${PROJECT_SOURCE_DIR}/test/test_pulp_vellum_param_bridge.cpp)
    target_link_libraries(pulp-test-pulp-vellum PRIVATE
        pulp::vellum Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum PROPERTIES LABELS pulp-vellum)
endif()
