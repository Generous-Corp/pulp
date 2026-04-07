cmake_minimum_required(VERSION 3.20)
project(test-daisysp CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  DaisySP
  GIT_REPOSITORY https://github.com/electro-smith/DaisySP.git
  GIT_TAG        V1.0.0
  GIT_SHALLOW    TRUE
)
FetchContent_Populate(DaisySP)

file(GLOB_RECURSE DAISYSP_SOURCES "${daisysp_SOURCE_DIR}/Source/**/*.cpp")
add_library(DaisySP STATIC ${DAISYSP_SOURCES})
# DaisySP uses flat includes across subdirectories
file(GLOB DAISYSP_SUBDIRS LIST_DIRECTORIES true "${daisysp_SOURCE_DIR}/Source/*")
target_include_directories(DaisySP PUBLIC "${daisysp_SOURCE_DIR}/Source")
foreach(subdir ${DAISYSP_SUBDIRS})
  if(IS_DIRECTORY ${subdir})
    target_include_directories(DaisySP PUBLIC ${subdir})
  endif()
endforeach()

add_executable(test-pkg main.cpp)
target_link_libraries(test-pkg PRIVATE DaisySP)
