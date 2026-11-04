cmake_minimum_required(VERSION 3.24)
project(PulpGraphLatencyPdcConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-g7-latency-pdc-consumer main.cpp)
target_compile_features(pulp-g7-latency-pdc-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-g7-latency-pdc-consumer PRIVATE Pulp::host)
enable_testing()
add_test(NAME g7-latency-pdc-consumer COMMAND pulp-g7-latency-pdc-consumer)
