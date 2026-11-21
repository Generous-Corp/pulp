find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-audio
        ${PULP_ROOT_DIR}/test/test_pulp_vellum_audio.cpp)
    target_link_libraries(pulp-test-pulp-vellum-audio PRIVATE
        pulp::vellum-audio Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-audio PROPERTIES LABELS pulp-vellum-audio)
endif()
