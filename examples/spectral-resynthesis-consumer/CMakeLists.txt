cmake_minimum_required(VERSION 3.24)
project(SpectralResynthesisConsumer LANGUAGES CXX)

# Analysis -> stage -> resynthesis, and a single decoded stream -> resynthesis,
# through the routed spectral graph, built only against the installed SDK's
# public Pulp::host target.
find_package(Pulp CONFIG REQUIRED COMPONENTS host)
if(NOT TARGET Pulp::host)
    message(FATAL_ERROR "The installed Pulp SDK did not export Pulp::host")
endif()
add_executable(spectral-resynthesis-consumer main.cpp)
target_compile_features(spectral-resynthesis-consumer PRIVATE cxx_std_20)
target_link_libraries(spectral-resynthesis-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME spectral-resynthesis-consumer COMMAND spectral-resynthesis-consumer)
# Planted geometry mutations: each must fail the named consumer check.
foreach(_plant IN ITEMS synthesis-hop window geometry-drift source-length source-count
                       source-drain)
    add_test(NAME spectral-resynthesis-consumer-planted-${_plant}
             COMMAND spectral-resynthesis-consumer --planted-${_plant})
endforeach()
set_tests_properties(spectral-resynthesis-consumer-planted-synthesis-hop PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL: stage -> synthesis geometry match")
set_tests_properties(spectral-resynthesis-consumer-planted-window PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL: geometry contract decode")
set_tests_properties(spectral-resynthesis-consumer-planted-geometry-drift PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL: reported latency N\\+H")
set_tests_properties(spectral-resynthesis-consumer-planted-source-length PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL: single-stream source admitted at its grid length")
set_tests_properties(spectral-resynthesis-consumer-planted-source-count PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL: source frame count == analysis grid")
set_tests_properties(spectral-resynthesis-consumer-planted-source-drain PROPERTIES
    PASS_REGULAR_EXPRESSION "FAIL: source drain == walked drain")
