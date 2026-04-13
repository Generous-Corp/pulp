add_executable(pulp-plugin-host-demo main.cpp)
target_link_libraries(pulp-plugin-host-demo PRIVATE pulp::host)
set_target_properties(pulp-plugin-host-demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/plugin-host-demo"
)
