cmake_minimum_required(VERSION 3.20)
project(test-signalsmith-stretch CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  signalsmith-stretch
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
  GIT_TAG        1.1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(signalsmith-stretch)

add_executable(test-pkg main.cpp)
target_include_directories(test-pkg PRIVATE ${signalsmith-stretch_SOURCE_DIR})
