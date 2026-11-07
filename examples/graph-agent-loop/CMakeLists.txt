cmake_minimum_required(VERSION 3.24)
project(PulpGraphAgentLoop LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-graph-agent-loop main.cpp)
target_compile_features(pulp-graph-agent-loop PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-agent-loop PRIVATE Pulp::host)

enable_testing()
add_test(NAME graph-agent-loop-consumer COMMAND pulp-graph-agent-loop)

