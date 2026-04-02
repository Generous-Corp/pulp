# FaustGain — FAUST DSL gain example
# Validates: FAUST offline codegen → Processor → headless test

if(PULP_BUILD_TESTS)
    add_executable(pulp-faust-gain-test test_faust_gain.cpp)
    target_link_libraries(pulp-faust-gain-test PRIVATE pulp::dsl pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-faust-gain-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-faust-gain-test)
endif()
