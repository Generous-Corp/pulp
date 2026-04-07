cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0177 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(test-libsamplerate C CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  libsamplerate
  GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
  GIT_TAG        0.2.2
  GIT_SHALLOW    TRUE
)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsamplerate)

add_executable(test-pkg main.cpp)
target_link_libraries(test-pkg PRIVATE SampleRate::samplerate)
