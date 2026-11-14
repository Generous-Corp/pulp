pulp_add_test_suite(pulp-test-prepared-description
    GROUP pulp-test-group-view-host-hosting
    LIBRARIES pulp::host
    LABELS "host;graph;g3;d4")

include("${CMAKE_CURRENT_LIST_DIR}/g3_resolved_identity_tests.cmake")
