find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-scene
        ${CMAKE_CURRENT_SOURCE_DIR}/draco_scene_adapter_test.cpp)
    target_link_libraries(pulp-test-pulp-vellum-scene PRIVATE
        pulp::vellum-scene Catch2::Catch2WithMain Threads::Threads)
    target_compile_definitions(pulp-test-pulp-vellum-scene PRIVATE
        PULP_VELLUM_SCENE_FIXTURE_DIR="${PULP_ROOT_DIR}/test/fixtures/scene3d")
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-scene PROPERTIES LABELS pulp-vellum-scene)
endif()
