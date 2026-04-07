cmake_minimum_required(VERSION 3.20)
project(test-signalsmith-dsp CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  signalsmith-dsp
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/dsp.git
  GIT_TAG        v1.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(signalsmith-dsp)

add_executable(test-pkg main.cpp)
target_include_directories(test-pkg PRIVATE ${signalsmith-dsp_SOURCE_DIR})
