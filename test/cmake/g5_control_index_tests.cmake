if(TARGET pulp-host)
    pulp_add_test_suite(pulp-test-g5-control-index
        SOURCES test_g5_control_index.cpp
        LIBRARIES pulp::host
        PROPERTIES RESOURCE_LOCK pulp_realtime_worker_pool)
    pulp_add_test_suite(pulp-test-control-scalar-parity
        SOURCES test_control_scalar_parity.cpp
        LIBRARIES pulp::host)
endif()

