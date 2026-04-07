cmake_minimum_required(VERSION 3.20)
project(test-r8brain CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  r8brain
  GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
  GIT_TAG        version-6.5
  GIT_SHALLOW    TRUE
)
FetchContent_Populate(r8brain)

add_library(r8brain STATIC "${r8brain_SOURCE_DIR}/r8bbase.cpp")
target_include_directories(r8brain PUBLIC "${r8brain_SOURCE_DIR}")

add_executable(test-pkg main.cpp)
target_link_libraries(test-pkg PRIVATE r8brain)
