add_executable(pulp-test-sample-region-parameters test_sample_region_parameters.cpp)
target_link_libraries(pulp-test-sample-region-parameters
    PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sample-region-parameters)
