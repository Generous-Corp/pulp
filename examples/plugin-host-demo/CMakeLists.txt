add_executable(pulp-plugin-host-demo main.cpp)
# pulp::platform provides ChildProcess, which reveal_in_file_manager now
# uses for a safe argv-based spawn.
target_link_libraries(pulp-plugin-host-demo PRIVATE pulp::host pulp::view pulp::platform)
set_target_properties(pulp-plugin-host-demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/plugin-host-demo"
)
