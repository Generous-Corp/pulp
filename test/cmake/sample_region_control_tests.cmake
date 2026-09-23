if(PULP_ENABLE_INSPECTOR AND TARGET pulp::inspect-runtime AND TARGET pulp::host)
    pulp_add_test_suite(pulp-test-control-sample-region
        LIBRARIES pulp::inspect-runtime pulp::host
        TIMEOUT 60)
endif()

function(_pulp_register_sample_region_control_e2e)
    if(NOT APPLE OR NOT TARGET sample-region-allpass-control-editable OR
       NOT TARGET sample-region-allpass-control-frozen OR NOT TARGET pulp-cli OR
       NOT TARGET pulp-mcp)
        return()
    endif()
    add_executable(pulp-test-control-sample-region-e2e
        "${CMAKE_SOURCE_DIR}/test/test_control_sample_region_e2e.cpp"
        "${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp")
    target_include_directories(pulp-test-control-sample-region-e2e PRIVATE
        "${CMAKE_SOURCE_DIR}/inspect/src" "${CMAKE_SOURCE_DIR}/test")
    target_link_libraries(pulp-test-control-sample-region-e2e PRIVATE
        pulp::inspect-client Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-control-sample-region-e2e PRIVATE
        PULP_SAMPLE_REGION_EDITABLE_STANDALONE="$<TARGET_FILE:sample-region-allpass-control-editable>"
        PULP_SAMPLE_REGION_FROZEN_STANDALONE="$<TARGET_FILE:sample-region-allpass-control-frozen>")
    set_target_properties(pulp-test-control-sample-region-e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/sample-region-control")
    add_dependencies(pulp-test-control-sample-region-e2e
        sample-region-allpass-control-editable sample-region-allpass-control-frozen
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
        VERBATIM)
    catch_discover_tests(pulp-test-control-sample-region-e2e
        PROPERTIES TIMEOUT 180 LABELS "inspect;control;sample-region;e2e")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_register_sample_region_control_e2e)
