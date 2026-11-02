cmake_minimum_required(VERSION 3.24)
project(PulpGraphHello LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-graph-hello main.cpp)
target_compile_features(pulp-graph-hello PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-hello PRIVATE Pulp::host)

enable_testing()
add_test(NAME graph-hello-positive COMMAND pulp-graph-hello)
set_tests_properties(graph-hello-positive PROPERTIES LABELS "examples;installed-sdk;graph")
