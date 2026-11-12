cmake_minimum_required(VERSION 3.24)
project(PulpG11RecordingPublication LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS audio host)

add_executable(pulp-g11-recording-publication main.cpp)
target_compile_features(pulp-g11-recording-publication PRIVATE cxx_std_20)
target_link_libraries(pulp-g11-recording-publication PRIVATE Pulp::audio Pulp::host)

enable_testing()
add_test(NAME g11-recording-publication-positive COMMAND pulp-g11-recording-publication)
add_test(NAME g11-recording-publication-two-take COMMAND pulp-g11-recording-publication --two-take)
add_test(NAME g11-recording-publication-wrong-buffer
    COMMAND pulp-g11-recording-publication --planted-wrong-buffer)
add_test(NAME g11-recording-publication-timing
    COMMAND pulp-g11-recording-publication --planted-timing)
add_test(NAME g11-recording-publication-stale
    COMMAND pulp-g11-recording-publication --planted-stale-publication)
set_tests_properties(g11-recording-publication-positive PROPERTIES LABELS "examples;installed-sdk;graph;g11")
set_tests_properties(g11-recording-publication-two-take PROPERTIES LABELS "examples;installed-sdk;graph;g11")
# Planted controls exit 0 only when the oracle rejects the planted defect, and
# must print the exact detection marker; a setup failure, crash or usage error
# therefore fails the test instead of passing it.
set_tests_properties(g11-recording-publication-wrong-buffer g11-recording-publication-timing
    g11-recording-publication-stale PROPERTIES LABELS "examples;installed-sdk;graph;g11;negative")
set_tests_properties(g11-recording-publication-wrong-buffer PROPERTIES
    PASS_REGULAR_EXPRESSION "G11 planted defect detected: wrong-buffer")
set_tests_properties(g11-recording-publication-timing PROPERTIES
    PASS_REGULAR_EXPRESSION "G11 planted defect detected: timing")
set_tests_properties(g11-recording-publication-stale PROPERTIES
    PASS_REGULAR_EXPRESSION "G11 planted defect detected: stale-publication")
