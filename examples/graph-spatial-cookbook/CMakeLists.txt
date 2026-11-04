cmake_minimum_required(VERSION 3.24)
project(PulpGraphSpatialCookbook LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-graph-spatial-cookbook main.cpp)
target_compile_features(pulp-graph-spatial-cookbook PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-spatial-cookbook PRIVATE Pulp::host)

enable_testing()
add_test(NAME graph-spatial-cookbook COMMAND pulp-graph-spatial-cookbook)
set_tests_properties(graph-spatial-cookbook PROPERTIES LABELS "examples;installed-sdk;graph")
