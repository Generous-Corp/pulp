# FaustFilter — FAUST DSL filter example
# Validates: multi-parameter reflection, biquad lowpass processing

if(PULP_BUILD_TESTS)
    add_executable(pulp-faust-filter-test test_faust_filter.cpp)
    target_link_libraries(pulp-faust-filter-test PRIVATE pulp::dsl pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-faust-filter-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-faust-filter-test)
endif()
