cmake_minimum_required(VERSION 3.20)
project(test-rtneural CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  RTNeural
  GIT_REPOSITORY https://github.com/jatinchowdhury18/RTNeural.git
  GIT_TAG        1fb1f075
)
FetchContent_MakeAvailable(RTNeural)

add_executable(test-pkg main.cpp)
target_link_libraries(test-pkg PRIVATE RTNeural)
