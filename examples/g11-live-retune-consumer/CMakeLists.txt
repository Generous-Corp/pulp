cmake_minimum_required(VERSION 3.24)
project(PulpG11LiveRetune LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host midi signal audio-analysis)
find_package(Threads REQUIRED)

foreach(_target IN ITEMS pulp-g11-live-retune pulp-g11-live-retune-mutation)
    add_executable(${_target} main.cpp)
    target_compile_features(${_target} PRIVATE cxx_std_20)
    target_link_libraries(${_target} PRIVATE Pulp::host Pulp::midi Pulp::signal
        Pulp::audio-analysis Threads::Threads)
endforeach()
target_compile_definitions(pulp-g11-live-retune-mutation PRIVATE G11_MUTATE_IGNORE_WRITE=1)

enable_testing()
add_test(NAME g11-live-retune-positive COMMAND pulp-g11-live-retune)
foreach(_control IN ITEMS late reset sibling mutation)
    if(_control STREQUAL "mutation")
        set(_probe pulp-g11-live-retune-mutation)
        set(_argument "")
        set(_reason "target phase oracle mismatch|target audio oracle mismatch")
    else()
        set(_probe pulp-g11-live-retune)
        set(_argument "--planted-${_control}")
        if(_control STREQUAL "sibling")
            set(_reason "sibling phase oracle mismatch|sibling audio oracle mismatch")
        elseif(_control STREQUAL "reset")
            set(_reason "target audio oracle mismatch|instrument lifecycle/envelope changed")
        else()
            set(_reason "target phase oracle mismatch|target audio oracle mismatch")
        endif()
    endif()
    add_test(NAME g11-live-retune-${_control}
        COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:${_probe}>
            "-DARGUMENT=${_argument}" "-DREASON=${_reason}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_refusal.cmake)
endforeach()
set_tests_properties(g11-live-retune-positive g11-live-retune-late g11-live-retune-reset
    g11-live-retune-sibling g11-live-retune-mutation PROPERTIES
    LABELS "examples;installed-sdk;graph;g11;tuning" TIMEOUT 180)
