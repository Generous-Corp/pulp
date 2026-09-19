add_executable(pulp-test-sample-region-authoring test_sample_region_authoring.cpp)
target_link_libraries(pulp-test-sample-region-authoring
    PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sample-region-authoring)
