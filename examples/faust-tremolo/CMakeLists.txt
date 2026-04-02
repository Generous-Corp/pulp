# FaustTremolo — FAUST DSL tremolo effect example
# Validates: modulation effect, phase-based oscillator in generated DSP

if(PULP_BUILD_TESTS)
    add_executable(pulp-faust-tremolo-test test_faust_tremolo.cpp)
    target_link_libraries(pulp-faust-tremolo-test PRIVATE pulp::dsl pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-faust-tremolo-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-faust-tremolo-test)
endif()
