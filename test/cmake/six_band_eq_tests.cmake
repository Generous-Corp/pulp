pulp_add_test_suite(pulp-test-six-band-eq
    SOURCES test_six_band_eq.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
