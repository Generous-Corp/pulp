cmake_minimum_required(VERSION 3.24)
project(PulpFaustRtSdkConsumer LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS dsl)

add_executable(pulp-faust-rt-sdk-consumer main.cpp)
target_link_libraries(pulp-faust-rt-sdk-consumer PRIVATE Pulp::dsl)
