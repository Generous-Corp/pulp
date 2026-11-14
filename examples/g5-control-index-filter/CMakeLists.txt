cmake_minimum_required(VERSION 3.24)
project(PulpG5ControlIndexFilter LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-g5-control-index-filter main.cpp)
target_compile_features(pulp-g5-control-index-filter PRIVATE cxx_std_20)
target_link_libraries(pulp-g5-control-index-filter PRIVATE Pulp::host)

enable_testing()
add_test(NAME g5-control-index-filter-positive COMMAND pulp-g5-control-index-filter)
add_test(NAME g5-control-index-filter-missing-modulation
    COMMAND pulp-g5-control-index-filter --planted-missing-modulation)
add_test(NAME g5-control-index-filter-swapped-index
    COMMAND pulp-g5-control-index-filter --planted-swapped-index)
set_tests_properties(g5-control-index-filter-positive PROPERTIES
    LABELS "examples;installed-sdk;graph;g5"
    PASS_REGULAR_EXPRESSION "G5 control-index filter verified")
set_tests_properties(g5-control-index-filter-missing-modulation PROPERTIES
    LABELS "examples;installed-sdk;graph;g5;negative"
    PASS_REGULAR_EXPRESSION "G5 planted defect detected: missing-modulation")
set_tests_properties(g5-control-index-filter-swapped-index PROPERTIES
    LABELS "examples;installed-sdk;graph;g5;negative"
    PASS_REGULAR_EXPRESSION "G5 planted defect detected: swapped-index")
