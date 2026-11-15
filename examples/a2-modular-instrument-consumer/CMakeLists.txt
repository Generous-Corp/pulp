cmake_minimum_required(VERSION 3.24)
project(PulpA2ModularInstrumentConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-a2-modular-instrument main.cpp)
target_compile_features(pulp-a2-modular-instrument PRIVATE cxx_std_20)
target_link_libraries(pulp-a2-modular-instrument PRIVATE Pulp::host)

enable_testing()
add_test(NAME a2-modular-instrument-positive COMMAND pulp-a2-modular-instrument)
set_tests_properties(a2-modular-instrument-positive PROPERTIES
    LABELS "examples;installed-sdk;graph;a2"
    PASS_REGULAR_EXPRESSION "A2 modular instrument verified")
foreach(_defect unknown-template out-of-range-level edited-level skewed-clock)
    add_test(NAME a2-modular-instrument-${_defect}
        COMMAND pulp-a2-modular-instrument --planted-${_defect})
    set_tests_properties(a2-modular-instrument-${_defect} PROPERTIES
        LABELS "examples;installed-sdk;graph;a2;negative"
        PASS_REGULAR_EXPRESSION "A2 planted defect detected: ${_defect}")
endforeach()
