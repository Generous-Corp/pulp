cmake_minimum_required(VERSION 3.24)
project(SpectralFrameCodecConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS signal)

# This offline codec is a public, header-only signal contract. Linking the
# installed interface target proves the consumer sees the same public include
# closure as an SDK project, without pulling in a private source-tree path.
if(NOT TARGET Pulp::signal)
    message(FATAL_ERROR "The installed Pulp SDK did not export Pulp::signal")
endif()
add_executable(spectral-frame-codec-consumer main.cpp)
target_compile_features(spectral-frame-codec-consumer PRIVATE cxx_std_20)
target_link_libraries(spectral-frame-codec-consumer PRIVATE Pulp::signal)
enable_testing()
add_test(NAME spectral-frame-codec-consumer COMMAND spectral-frame-codec-consumer)
add_test(NAME spectral-frame-codec-consumer-planted-unknown-profile
         COMMAND spectral-frame-codec-consumer --planted-unknown-profile)
set_tests_properties(spectral-frame-codec-consumer-planted-unknown-profile
                     PROPERTIES WILL_FAIL TRUE)
