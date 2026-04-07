cmake_minimum_required(VERSION 3.20)
project(test-pffft C CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  pffft
  GIT_REPOSITORY https://github.com/marton78/pffft.git
  GIT_TAG        v1.1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pffft)

add_executable(test-pkg main.cpp)
target_link_libraries(test-pkg PRIVATE PFFFT)
