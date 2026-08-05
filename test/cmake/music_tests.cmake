pulp_add_test_suite(pulp-test-music-theory LIBRARIES pulp::music)
pulp_add_test_suite(pulp-test-music-generative LIBRARIES pulp::music)
pulp_add_test_suite(pulp-test-music-compatibility
    LIBRARIES pulp::music pulp::signal pulp::timeline
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/core/timeline/src)
