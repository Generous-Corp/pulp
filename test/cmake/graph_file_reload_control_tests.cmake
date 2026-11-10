function(_pulp_register_graph_file_reload_control_e2e)
if(PULP_ENABLE_INSPECTOR AND APPLE AND NOT IOS AND NOT PULP_IOS AND
   TARGET pulp::inspect-standalone-runtime AND TARGET pulp-cli AND TARGET pulp-mcp)
    find_program(_pulp_graph_file_reload_codesign codesign REQUIRED)
    add_executable(pulp-control-graph_file_reload-graph-product-fixture
        ${CMAKE_SOURCE_DIR}/test/fixtures/control_graph_file_reload_fixture.cpp)
    target_link_libraries(pulp-control-graph_file_reload-graph-product-fixture PRIVATE
        pulp::inspect-standalone-runtime pulp::standalone pulp::format pulp::host)
    target_link_options(pulp-control-graph_file_reload-graph-product-fixture PRIVATE LINKER:-dead_strip_dylibs)
    _pulp_cache_control_declarations(pulp-control-graph_file_reload-graph-product-fixture developer-local
        "dev.pulp.instance/read@1;dev.pulp.session/control@1;dev.pulp.graph/file.reload@1"
        FALSE)
    _pulp_configure_control_shipping(pulp-control-graph_file_reload-graph-product-fixture
        "dev.pulp.test.graph-file-reload" "DSPX-04 Graph Product Fixture")
    _pulp_attach_control_shipping(pulp-control-graph_file_reload-graph-product-fixture
        pulp-control-graph_file_reload-graph-product-fixture Standalone)
    add_custom_command(TARGET pulp-control-graph_file_reload-graph-product-fixture POST_BUILD
        COMMAND "${_pulp_graph_file_reload_codesign}" --force --sign - --options library
                "$<TARGET_FILE:pulp-control-graph_file_reload-graph-product-fixture>"
        COMMAND /bin/chmod 0700 "$<TARGET_FILE:pulp-control-graph_file_reload-graph-product-fixture>"
        COMMAND /bin/chmod 0600
                "$<TARGET_FILE:pulp-control-graph_file_reload-graph-product-fixture>.inspector-capabilities.json"
        VERBATIM)

    add_executable(pulp-test-control-graph_file_reload-graph-product-e2e
        ${CMAKE_SOURCE_DIR}/test/test_control_graph_file_reload_e2e.cpp
        ${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp)
    target_include_directories(pulp-test-control-graph_file_reload-graph-product-e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/inspect/src ${CMAKE_SOURCE_DIR}/test
        ${CMAKE_SOURCE_DIR}/core/audio/include)
    target_link_libraries(pulp-test-control-graph_file_reload-graph-product-e2e PRIVATE
        pulp::inspect-client pulp::audio Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-control-graph_file_reload-graph-product-e2e PRIVATE
        PULP_GRAPH_FILE_RELOAD_GRAPH_PRODUCT_FIXTURE="$<TARGET_FILE:pulp-control-graph_file_reload-graph-product-fixture>" PULP_GRAPH_FILE_RELOAD_PROJECT_ROOT="${CMAKE_BINARY_DIR}/test/graph-file-reload-project" PULP_SAMPLE_REGION_EDITABLE_STANDALONE="" PULP_SAMPLE_REGION_FROZEN_STANDALONE="")
    pulp_test_spawns(pulp-test-control-graph_file_reload-graph-product-e2e
        pulp-cli pulp-mcp pulp-control-graph_file_reload-graph-product-fixture)
    add_dependencies(pulp-test-control-graph_file_reload-graph-product-e2e
        pulp-control-graph_file_reload-graph-product-fixture pulp-cli pulp-mcp)
    set_target_properties(pulp-test-control-graph_file_reload-graph-product-e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/graph_file_reload-control")
    add_custom_command(TARGET pulp-test-control-graph_file_reload-graph-product-e2e POST_BUILD
        COMMAND "${_pulp_graph_file_reload_codesign}" --force --sign -
                "$<TARGET_FILE:pulp-test-control-graph_file_reload-graph-product-e2e>"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-cli>"
                "$<TARGET_FILE_DIR:pulp-test-control-graph_file_reload-graph-product-e2e>/pulp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-mcp>"
                "$<TARGET_FILE_DIR:pulp-test-control-graph_file_reload-graph-product-e2e>/pulp-mcp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-test-control-graph_file_reload-graph-product-e2e>"
                "$<TARGET_FILE_DIR:pulp-test-control-graph_file_reload-graph-product-e2e>/pulp-control-broker"
        # The broker trusts a client copy beside itself. It must sit inside this
        # test's own directory: a shared ../tools/cli path is overwritten by
        # any other control e2e target, and this test then fails enrollment.
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:pulp-test-control-graph_file_reload-graph-product-e2e>/tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-test-control-graph_file_reload-graph-product-e2e>"
                "$<TARGET_FILE_DIR:pulp-test-control-graph_file_reload-graph-product-e2e>/tools/cli/pulp-cpp"
        VERBATIM)
    pulp_scaled_test_timeout(_pulp_graph_file_reload_graph_product_timeout 600)
    catch_discover_tests(pulp-test-control-graph_file_reload-graph-product-e2e
        PROPERTIES TIMEOUT "${_pulp_graph_file_reload_graph_product_timeout}"
                  LABELS "inspect;control;graph-file-reload;e2e;product")
endif()

endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_register_graph_file_reload_control_e2e)
