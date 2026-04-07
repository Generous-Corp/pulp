cmake_minimum_required(VERSION 3.20)
project(test-cycfi-q CXX)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
FetchContent_Declare(
  q
  GIT_REPOSITORY https://github.com/cycfi/q.git
  GIT_TAG        version_1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(q)

add_executable(test-pkg main.cpp)
target_link_libraries(test-pkg PRIVATE libq)
