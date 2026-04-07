cmake_minimum_required(VERSION 3.20)
project(test-dr-libs CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG        wav-0.14.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(dr_libs)

add_executable(test-pkg main.cpp)
target_include_directories(test-pkg PRIVATE ${dr_libs_SOURCE_DIR})
