# pulp CLI tool
add_executable(pulp-cli pulp_cli.cpp)
set_target_properties(pulp-cli PROPERTIES OUTPUT_NAME "pulp")
target_compile_features(pulp-cli PRIVATE cxx_std_20)

# Install to project root for easy access
install(TARGETS pulp-cli DESTINATION "${CMAKE_SOURCE_DIR}")
