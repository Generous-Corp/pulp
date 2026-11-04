cmake_minimum_required(VERSION 3.24)
project(SpectralFrameCodecConsumer LANGUAGES CXX)

# This offline header has no Pulp library or private-header dependency.
find_path(PULP_SIGNAL_INCLUDE_DIR pulp/signal/spectral_frame_codec.hpp REQUIRED)
add_executable(spectral-frame-codec-consumer main.cpp)
target_compile_features(spectral-frame-codec-consumer PRIVATE cxx_std_20)
target_include_directories(spectral-frame-codec-consumer PRIVATE ${PULP_SIGNAL_INCLUDE_DIR})
enable_testing()
add_test(NAME spectral-frame-codec-consumer COMMAND spectral-frame-codec-consumer)
