if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(PulpAudioRateProcessorNodeExample LANGUAGES CXX)
    find_package(Pulp REQUIRED COMPONENTS format graph)
endif()

add_executable(pulp-audio-rate-processor-node main.cpp)
target_compile_features(pulp-audio-rate-processor-node PRIVATE cxx_std_20)
target_link_libraries(pulp-audio-rate-processor-node PRIVATE pulp::format pulp::graph)

if(PULP_BUILD_TESTS)
    add_test(NAME audio-rate-processor-node-example
        COMMAND pulp-audio-rate-processor-node)
    set_tests_properties(audio-rate-processor-node-example PROPERTIES
        LABELS "examples;format;graph;audio-rate" TIMEOUT 10)
endif()
