function(_pulp_register_dspx04_control_e2e)
if(PULP_ENABLE_INSPECTOR AND APPLE AND NOT IOS AND NOT PULP_IOS AND
   TARGET pulp::inspect-standalone-runtime AND TARGET pulp-cli AND TARGET pulp-mcp)
    find_program(_pulp_dspx04_codesign codesign REQUIRED)
    add_executable(pulp-control-dspx04-graph-product-fixture
        ${CMAKE_SOURCE_DIR}/test/fixtures/control_dspx04_graph_product_fixture.cpp)
    target_link_libraries(pulp-control-dspx04-graph-product-fixture PRIVATE
        pulp::inspect-standalone-runtime pulp::standalone pulp::format pulp::host)
    target_link_options(pulp-control-dspx04-graph-product-fixture PRIVATE LINKER:-dead_strip_dylibs)
    _pulp_cache_control_declarations(pulp-control-dspx04-graph-product-fixture developer-local
        "dev.pulp.instance/read@1;dev.pulp.session/control@1;dev.pulp.graph/modulation-route.edit@1"
        FALSE)
    _pulp_configure_control_shipping(pulp-control-dspx04-graph-product-fixture
        "dev.pulp.test.dspx04-graph-product" "DSPX-04 Graph Product Fixture")
    _pulp_attach_control_shipping(pulp-control-dspx04-graph-product-fixture
        pulp-control-dspx04-graph-product-fixture Standalone)
    add_custom_command(TARGET pulp-control-dspx04-graph-product-fixture POST_BUILD
        COMMAND "${_pulp_dspx04_codesign}" --force --sign - --options library
                "$<TARGET_FILE:pulp-control-dspx04-graph-product-fixture>"
        COMMAND /bin/chmod 0700 "$<TARGET_FILE:pulp-control-dspx04-graph-product-fixture>"
        COMMAND /bin/chmod 0600
                "$<TARGET_FILE:pulp-control-dspx04-graph-product-fixture>.inspector-capabilities.json"
        VERBATIM)

    add_executable(pulp-test-control-dspx04-graph-product-e2e
        ${CMAKE_SOURCE_DIR}/test/test_control_dspx04_graph_product_e2e.cpp
        ${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp)
    target_include_directories(pulp-test-control-dspx04-graph-product-e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/inspect/src ${CMAKE_SOURCE_DIR}/test
        ${CMAKE_SOURCE_DIR}/core/audio/include)
    target_link_libraries(pulp-test-control-dspx04-graph-product-e2e PRIVATE
        pulp::inspect-client pulp::audio Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-control-dspx04-graph-product-e2e PRIVATE
        PULP_DSPX04_GRAPH_PRODUCT_FIXTURE="$<TARGET_FILE:pulp-control-dspx04-graph-product-fixture>" PULP_SAMPLE_REGION_EDITABLE_STANDALONE="" PULP_SAMPLE_REGION_FROZEN_STANDALONE="")
    add_dependencies(pulp-test-control-dspx04-graph-product-e2e
        pulp-control-dspx04-graph-product-fixture pulp-cli pulp-mcp)
    set_target_properties(pulp-test-control-dspx04-graph-product-e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/dspx04-control")
    add_custom_command(TARGET pulp-test-control-dspx04-graph-product-e2e POST_BUILD
        COMMAND "${_pulp_dspx04_codesign}" --force --sign -
                "$<TARGET_FILE:pulp-test-control-dspx04-graph-product-e2e>"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-cli>"
                "$<TARGET_FILE_DIR:pulp-test-control-dspx04-graph-product-e2e>/pulp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-mcp>"
                "$<TARGET_FILE_DIR:pulp-test-control-dspx04-graph-product-e2e>/pulp-mcp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-test-control-dspx04-graph-product-e2e>"
                "$<TARGET_FILE_DIR:pulp-test-control-dspx04-graph-product-e2e>/pulp-control-broker"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:pulp-test-control-dspx04-graph-product-e2e>/../tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-test-control-dspx04-graph-product-e2e>"
                "$<TARGET_FILE_DIR:pulp-test-control-dspx04-graph-product-e2e>/../tools/cli/pulp-cpp"
        VERBATIM)
    pulp_scaled_test_timeout(_pulp_dspx04_graph_product_timeout 180)
    catch_discover_tests(pulp-test-control-dspx04-graph-product-e2e
        PROPERTIES TIMEOUT "${_pulp_dspx04_graph_product_timeout}"
                  LABELS "inspect;control;dspx-04;e2e;product")
endif()

# Keep the product contract discoverable on non-Apple builders.  The fixture
# launch is Apple-only, but the test's typed SKIP is itself a required control
# surface and must be visible to CTest on every supported host.
if(PULP_ENABLE_INSPECTOR AND NOT APPLE AND NOT IOS AND NOT PULP_IOS AND
   TARGET pulp::inspect-client AND TARGET pulp-cli AND TARGET pulp-mcp)
    add_executable(pulp-test-control-dspx04-graph-product-e2e
        ${CMAKE_SOURCE_DIR}/test/test_control_dspx04_graph_product_typed_skip.cpp)
    target_link_libraries(pulp-test-control-dspx04-graph-product-e2e PRIVATE
        Catch2::Catch2WithMain)
    set_target_properties(pulp-test-control-dspx04-graph-product-e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/dspx04-control")
    pulp_scaled_test_timeout(_pulp_dspx04_graph_product_nonapple_timeout 30)
    catch_discover_tests(pulp-test-control-dspx04-graph-product-e2e
        PROPERTIES TIMEOUT "${_pulp_dspx04_graph_product_nonapple_timeout}"
                  LABELS "inspect;control;dspx-04;e2e;product;typed-skip")
endif()

# Keep the typed-skip product contract discoverable when a platform configure
# omits the inspector SDK or one of its product dependencies.  The real Apple
# receipt above and the non-Apple inspector build both own this target name;
# this fallback only fills the registration gap with the same typed-skip test.
if(PULP_BUILD_TESTS AND NOT TARGET pulp-test-control-dspx04-graph-product-e2e AND
   TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-control-dspx04-graph-product-e2e
        ${CMAKE_SOURCE_DIR}/test/test_control_dspx04_graph_product_typed_skip.cpp)
    target_link_libraries(pulp-test-control-dspx04-graph-product-e2e PRIVATE
        Catch2::Catch2WithMain)
    pulp_scaled_test_timeout(_pulp_dspx04_graph_product_fallback_timeout 30)
    catch_discover_tests(pulp-test-control-dspx04-graph-product-e2e
        PROPERTIES TIMEOUT "${_pulp_dspx04_graph_product_fallback_timeout}"
                  LABELS "inspect;control;dspx-04;e2e;product;typed-skip")
endif()

endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_register_dspx04_control_e2e)
