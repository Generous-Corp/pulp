# Focused sample-region runtime acceptance. Keep this target independently
# selectable: R2 validation runs its acceptance families by Catch tag instead
# of sweeping the broader graph/audio suites.
pulp_add_test_suite(pulp-test-sample-region-runtime
    SOURCES
        test_sample_region_runtime.cpp
        sample_region_i2/i2_abi_resource_contract.cpp
        sample_region_i2/i2_negative_controls.cpp
        harness/rt_allocation_probe.cpp
        support/render_scenario.cpp
        support/audio_signal_generators.cpp
    LIBRARIES
        pulp::host
        pulp::format
        pulp::graph
        pulp::audio
        pulp::audio-analysis)

if(Python3_EXECUTABLE)
    add_test(NAME sample-region-i2-rt02-process-contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/sample_region_i2/rt02_process_contract.py
            --repo-root ${PROJECT_SOURCE_DIR}
            --negative-control)
    set_tests_properties(sample-region-i2-rt02-process-contract PROPERTIES
        LABELS "sample-region;i2;rt-safety")
endif()
