cmake_minimum_required(VERSION 3.24)
project(PulpG6VoiceContract LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS midi)
add_executable(pulp-g6-voice-contract main.cpp)
target_compile_features(pulp-g6-voice-contract PRIVATE cxx_std_20)
target_link_libraries(pulp-g6-voice-contract PRIVATE Pulp::midi)

enable_testing()
add_test(NAME g6-allocator-contract COMMAND pulp-g6-voice-contract)
set_tests_properties(g6-allocator-contract PROPERTIES
    LABELS "examples;installed-sdk;midi" TIMEOUT 10)
