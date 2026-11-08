cmake_minimum_required(VERSION 3.24)
project(PulpG4VoiceBankConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-g4-voice-bank-consumer main.cpp)
target_compile_features(pulp-g4-voice-bank-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-g4-voice-bank-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME g4-voice-bank-public-consumer COMMAND pulp-g4-voice-bank-consumer)
set_tests_properties(g4-voice-bank-public-consumer PROPERTIES
    LABELS "examples;installed-sdk;host" TIMEOUT 30)
