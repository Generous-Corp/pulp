cmake_minimum_required(VERSION 3.24)
project(A10RoutingHostingConsumer LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS host)
add_executable(a10-routing-hosting-consumer main.cpp)
target_compile_features(a10-routing-hosting-consumer PRIVATE cxx_std_20)
target_link_libraries(a10-routing-hosting-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME a10-routing-hosting-public-consumer COMMAND a10-routing-hosting-consumer)
