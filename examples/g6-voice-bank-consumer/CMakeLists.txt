cmake_minimum_required(VERSION 3.24)
project(PulpG6VoiceBankConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-g6-voice-bank-consumer main.cpp)
target_compile_features(pulp-g6-voice-bank-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-g6-voice-bank-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME g6-voice-bank-public-consumer COMMAND pulp-g6-voice-bank-consumer)
set_tests_properties(g6-voice-bank-public-consumer PROPERTIES
    LABELS "examples;installed-sdk;host;graph" TIMEOUT 30)
