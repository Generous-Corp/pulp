add_executable(pulp-plugin-host-demo main.cpp)
# pulp::platform provides ChildProcess, which reveal_in_file_manager now
# uses for a safe argv-based spawn.
target_link_libraries(pulp-plugin-host-demo PRIVATE
    pulp::events pulp::host pulp::view pulp::platform)
set_target_properties(pulp-plugin-host-demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/plugin-host-demo"
)

# Arg-handling regression guard: --help documents the --editor flag. The editor
# actually rendering on screen needs a display and a real plugin with a GUI, so
# that is proven by the manual DAW smoke, not here.
if(PULP_BUILD_TESTS)
    add_test(NAME plugin-host-demo-editor-help
             COMMAND pulp-plugin-host-demo --help)
    set_tests_properties(plugin-host-demo-editor-help PROPERTIES
        PASS_REGULAR_EXPRESSION "--editor")
endif()
