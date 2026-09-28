if(PULP_ENABLE_INSPECTOR AND TARGET pulp::inspect-sample-region-runtime)
    pulp_add_test_suite(pulp-test-control-sample-region
        LIBRARIES pulp::inspect-sample-region-runtime
        TIMEOUT 60)
endif()

function(_pulp_register_sample_region_control_e2e)
    if(NOT APPLE OR NOT TARGET sample-region-allpass-control-editable OR
       NOT TARGET sample-region-allpass-control-frozen OR NOT TARGET pulp-cli OR
       NOT TARGET pulp-mcp)
        return()
    endif()
    add_executable(pulp-control-sample-region-proof-host
        "${CMAKE_SOURCE_DIR}/test/test_control_sample_region_e2e.cpp")
    target_compile_definitions(pulp-control-sample-region-proof-host PRIVATE
        PULP_SAMPLE_REGION_BROKER_PROOF_HOST=1)
    target_link_libraries(pulp-control-sample-region-proof-host PRIVATE
        sample-region-allpass-core pulp::inspect-standalone-runtime pulp::standalone pulp::format)
    _pulp_cache_control_declarations(pulp-control-sample-region-proof-host developer-local
        "dev.pulp.instance/read@1;dev.pulp.session/control@1;dev.pulp.state/read@1;dev.pulp.state/parameter-gesture@1;dev.pulp.graph/sample-region.read@1;dev.pulp.graph/sample-region.edit@1"
        FALSE)
    _pulp_configure_control_shipping(pulp-control-sample-region-proof-host
        "dev.pulp.sample-region-allpass.proof" "Sample Region Broker Proof")
    _pulp_attach_control_shipping(pulp-control-sample-region-proof-host
        pulp-control-sample-region-proof-host Standalone)
    target_link_options(pulp-control-sample-region-proof-host PRIVATE LINKER:-dead_strip_dylibs)
    find_program(_sample_region_test_codesign codesign REQUIRED)
    add_custom_command(TARGET pulp-control-sample-region-proof-host POST_BUILD
        COMMAND "${_sample_region_test_codesign}" --force --sign - --options library
            "$<TARGET_FILE:pulp-control-sample-region-proof-host>"
        COMMAND chmod 0700 "$<TARGET_FILE:pulp-control-sample-region-proof-host>"
        COMMAND chmod 0600
            "$<TARGET_FILE:pulp-control-sample-region-proof-host>.inspector-capabilities.json"
        VERBATIM)
    add_executable(pulp-test-control-sample-region-e2e
        "${CMAKE_SOURCE_DIR}/test/test_control_sample_region_e2e.cpp"
        "${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp")
    target_include_directories(pulp-test-control-sample-region-e2e PRIVATE
        "${CMAKE_SOURCE_DIR}/inspect/src" "${CMAKE_SOURCE_DIR}/test")
    target_link_libraries(pulp-test-control-sample-region-e2e PRIVATE
        pulp::inspect-client pulp::audio Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-control-sample-region-e2e PRIVATE
        PULP_SAMPLE_REGION_EDITABLE_STANDALONE="$<TARGET_FILE:sample-region-allpass-control-editable>"
        PULP_SAMPLE_REGION_FROZEN_STANDALONE="$<TARGET_FILE:sample-region-allpass-control-frozen>"
        PULP_SAMPLE_REGION_PROOF_HOST="$<TARGET_FILE:pulp-control-sample-region-proof-host>")
    set_target_properties(pulp-test-control-sample-region-e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/sample-region-control")
    add_dependencies(pulp-test-control-sample-region-e2e
        sample-region-allpass-control-editable sample-region-allpass-control-frozen
        pulp-control-sample-region-proof-host
        pulp-cli pulp-mcp)
    find_program(_sample_region_test_codesign codesign REQUIRED)
    add_custom_command(TARGET pulp-test-control-sample-region-e2e POST_BUILD
        COMMAND "${_sample_region_test_codesign}" --force --sign -
            "$<TARGET_FILE:pulp-test-control-sample-region-e2e>"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-cli>"
            "$<TARGET_FILE_DIR:pulp-test-control-sample-region-e2e>/pulp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-mcp>"
            "$<TARGET_FILE_DIR:pulp-test-control-sample-region-e2e>/pulp-mcp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-test-control-sample-region-e2e>"
            "$<TARGET_FILE_DIR:pulp-test-control-sample-region-e2e>/pulp-control-broker"
        # Management-client alias. ControlBrokerDaemon derives its trusted-client
        # set by statically inspecting `pulp`, `pulp-cpp` and `pulp-mcp` at fixed
        # paths around the configured executable, then admits an enrolling peer
        # only when the peer's (executable_identity, publisher_id) matches one of
        # them. This test IS the enrolling client, so its own identity has to be
        # in that set or `authorize_client` rejects it and enroll returns
        # "enrollment-denied". The daemon's executable_path must equal the running
        # executable, so the search is anchored on this directory and cannot be
        # redirected.
        #
        # The staged `pulp` is the production C++ `pulp-cli` target. Keep the
        # management-client alias at `<bin>/../tools/cli/pulp-cpp` so the broker
        # can trust the enrolling test executable without replacing that CLI.
        #
        # Copied, never re-signed: ad-hoc signing derives the identifier from the
        # basename, so re-signing under another name would change the very
        # identity the alias exists to carry. Same reasoning as the
        # `stage_signed_binary(..., false)` aliases in
        # test_control_phase15_aggregate_e2e.cpp.
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:pulp-test-control-sample-region-e2e>/../tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-test-control-sample-region-e2e>"
            "$<TARGET_FILE_DIR:pulp-test-control-sample-region-e2e>/../tools/cli/pulp-cpp"
        VERBATIM)
    pulp_scaled_test_timeout(_pulp_sample_region_e2e_timeout 180)
    catch_discover_tests(pulp-test-control-sample-region-e2e
        PROPERTIES TIMEOUT "${_pulp_sample_region_e2e_timeout}"
        LABELS "inspect;control;sample-region;e2e")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_register_sample_region_control_e2e)
