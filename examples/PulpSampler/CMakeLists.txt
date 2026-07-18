# PulpSampler — sample-buffer sampler with MIDI triggering and ADSR
# CLAP is the validated example format; enable others only after validation.

include(pulp_sampler_sources.cmake)

if(PULP_HAS_CLAP)
    # CLAP plugin
    add_library(PulpSampler_CLAP MODULE clap_entry.cpp ${PULP_SAMPLER_IMPLEMENTATION})
    target_link_libraries(PulpSampler_CLAP PRIVATE pulp::format pulp::audio pulp::signal clap)
    target_include_directories(PulpSampler_CLAP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(PulpSampler_CLAP PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        OUTPUT_NAME PulpSampler
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP"
    )
    if(APPLE)
        set_target_properties(PulpSampler_CLAP PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.pulp.sampler"
            MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        )
    endif()
endif()

# Test
if(PULP_BUILD_TESTS)
    add_executable(pulp-sampler-test
        ${PULP_SAMPLER_IMPLEMENTATION}
        test_pulp_sampler_contract.cpp
        test_pulp_sampler_mip_sidecar.cpp
        test_pulp_sampler_mip_runtime.cpp
        test_pulp_sampler_resident_rt.cpp
        test_pulp_sampler_streaming.cpp
        test_pulp_sampler_streaming_admission_rt.cpp
        test_pulp_sampler_heritage_render_clock.cpp
        test_pulp_sampler_heritage_stream_domain.cpp
        test_pulp_sampler_heritage_state_failure_rt.cpp
        test_sampler_api.cpp
        ${PROJECT_SOURCE_DIR}/test/support/sampler_parity.cpp
        ${PROJECT_SOURCE_DIR}/test/harness/rt_allocation_probe.cpp)
    target_link_libraries(pulp-sampler-test PRIVATE pulp::format pulp::audio pulp::signal Catch2::Catch2WithMain)
    target_compile_definitions(pulp-sampler-test PRIVATE PULP_SAMPLER_TEST_HOOKS=1)
    target_include_directories(pulp-sampler-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/test/harness)
    catch_discover_tests(pulp-sampler-test PROPERTIES TIMEOUT 30)

    add_executable(pulp-sampler-cli-sidecar-probe
        ${PULP_SAMPLER_IMPLEMENTATION}
        test_sampler_cli_sidecar_probe.cpp)
    target_link_libraries(pulp-sampler-cli-sidecar-probe PRIVATE
        pulp::format pulp::audio pulp::signal)
    target_include_directories(pulp-sampler-cli-sidecar-probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    if(TARGET pulp-test-cli-shellout)
        add_dependencies(pulp-test-cli-shellout
            pulp-sampler-cli-sidecar-probe)
        target_compile_definitions(pulp-test-cli-shellout PRIVATE
            PULP_SAMPLER_SIDECAR_PROBE_BINARY="$<TARGET_FILE:pulp-sampler-cli-sidecar-probe>")
    endif()
endif()
