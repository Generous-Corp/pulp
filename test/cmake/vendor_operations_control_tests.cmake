# The agent tool kit example is built here too, so its declaration helper,
# CONTROL_VENDOR_SOURCES, and controlled host are validated without examples.
if(PULP_ENABLE_INSPECTOR AND APPLE AND PULP_ENABLE_GPU AND NOT IOS AND NOT PULP_IOS AND
   TARGET pulp::inspect-agent-toolkit AND NOT TARGET AgentToolkitSynth)
    # This fixture is built as part of the test tree, but its controlled host
    # must not add a product install script to the producer SDK install. SDK
    # consumer tests stage that install under an unprivileged prefix; leaving
    # the fixture's install rule enabled makes it try to create the default
    # /usr/local/libexec catalog on macOS.
    set(_pulp_saved_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
    set(CMAKE_SKIP_INSTALL_RULES TRUE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/examples/agent-toolkit-synth"
        "${CMAKE_BINARY_DIR}/test/agent-toolkit-synth")
    set(CMAKE_SKIP_INSTALL_RULES "${_pulp_saved_skip_install_rules}")
    # The broker-launched host is built as a file named `host`, a name test
    # sources use as an ordinary word, so the test-input scanner would read it
    # as a program those tests spawn. Install renames the host to `host` under
    # its own directory either way; only this build-tree copy gets a distinct
    # name.
    set_target_properties(AgentToolkitSynth_ControlHost PROPERTIES
        OUTPUT_NAME agent-toolkit-synth-control-host)
endif()

# A real controlled Standalone product that declares vendor operations, driven
# through the broker by the shipped CLI and pulp-mcp.
function(_pulp_register_vendor_operations_control_e2e)
if(PULP_ENABLE_INSPECTOR AND APPLE AND NOT IOS AND NOT PULP_IOS AND
   TARGET pulp::inspect-standalone-runtime AND TARGET pulp-cli AND TARGET pulp-mcp)
    find_program(_pulp_vendor_codesign codesign REQUIRED)
    set(_fixture pulp-control-vendor-product-fixture)
    add_executable(${_fixture}
        ${CMAKE_SOURCE_DIR}/test/fixtures/control_vendor_product_fixture.cpp)
    target_link_libraries(${_fixture} PRIVATE
        pulp::inspect-standalone-runtime pulp::standalone pulp::format)
    target_link_options(${_fixture} PRIVATE LINKER:-dead_strip_dylibs)
    _pulp_cache_control_declarations(${_fixture} developer-local
        "dev.pulp.instance/read@1;dev.pulp.session/control@1;dev.pulp.vendor/read@1;dev.pulp.vendor/gesture@1;dev.pulp.vendor/render@1;dev.pulp.vendor/destructive@1"
        FALSE)
    _pulp_cache_control_vendor_operations(${_fixture}
        "${CMAKE_SOURCE_DIR}/test/fixtures/control_vendor_product_operations.json")
    _pulp_configure_control_shipping(${_fixture}
        "org.example.vendor-product" "Vendor Operations Product Fixture")
    _pulp_attach_control_shipping(${_fixture} ${_fixture} Standalone)
    add_custom_command(TARGET ${_fixture} POST_BUILD
        COMMAND "${_pulp_vendor_codesign}" --force --sign - --options library
                "$<TARGET_FILE:${_fixture}>"
        COMMAND /bin/chmod 0700 "$<TARGET_FILE:${_fixture}>"
        COMMAND /bin/chmod 0600 "$<TARGET_FILE:${_fixture}>.inspector-capabilities.json"
        VERBATIM)

    set(_e2e pulp-test-control-vendor-operations-e2e)
    add_executable(${_e2e}
        ${CMAKE_SOURCE_DIR}/test/test_control_vendor_operations_e2e.cpp
        ${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp)
    target_include_directories(${_e2e} PRIVATE
        ${CMAKE_SOURCE_DIR}/inspect/src ${CMAKE_SOURCE_DIR}/test
        ${CMAKE_SOURCE_DIR}/core/audio/include)
    target_link_libraries(${_e2e} PRIVATE pulp::inspect-client pulp::audio Catch2::Catch2WithMain)
    target_compile_definitions(${_e2e} PRIVATE
        PULP_VENDOR_PRODUCT_FIXTURE="$<TARGET_FILE:${_fixture}>"
        PULP_SAMPLE_REGION_EDITABLE_STANDALONE="" PULP_SAMPLE_REGION_FROZEN_STANDALONE="")
    add_dependencies(${_e2e} ${_fixture} pulp-cli pulp-mcp)
    if(TARGET AgentToolkitSynth_ControlHost)
        target_compile_definitions(${_e2e} PRIVATE
            PULP_AGENT_TOOLKIT_PRODUCT="$<TARGET_FILE:AgentToolkitSynth_ControlHost>")
        add_dependencies(${_e2e} AgentToolkitSynth_ControlHost)
    endif()
    set_target_properties(${_e2e} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/vendor-operations-control")
    add_custom_command(TARGET ${_e2e} POST_BUILD
        COMMAND "${_pulp_vendor_codesign}" --force --sign - "$<TARGET_FILE:${_e2e}>"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-cli>"
                "$<TARGET_FILE_DIR:${_e2e}>/pulp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:pulp-mcp>"
                "$<TARGET_FILE_DIR:${_e2e}>/pulp-mcp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${_e2e}>"
                "$<TARGET_FILE_DIR:${_e2e}>/pulp-control-broker"
        # Inside this test's own directory, never a shared ../tools/cli that
        # another control e2e target would overwrite.
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${_e2e}>/tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${_e2e}>"
                "$<TARGET_FILE_DIR:${_e2e}>/tools/cli/pulp-cpp"
        VERBATIM)
    pulp_scaled_test_timeout(_pulp_vendor_e2e_timeout 180)
    catch_discover_tests(${_e2e}
        PROPERTIES TIMEOUT "${_pulp_vendor_e2e_timeout}"
                  LABELS "inspect;control;vendor;e2e;product")
endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_register_vendor_operations_control_e2e)
