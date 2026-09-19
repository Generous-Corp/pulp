cmake_minimum_required(VERSION 3.25)
project(pulp_sample_region_compat LANGUAGES C CXX)

if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

set(PULP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PULP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${PULP_SOURCE_DIR}" pulp EXCLUDE_FROM_ALL)

add_executable(sample-region-compat-runner compat_runner.cpp)
target_compile_features(sample-region-compat-runner PRIVATE cxx_std_23)
target_link_libraries(sample-region-compat-runner PRIVATE pulp::host)

add_executable(sample-region-old-sdk-consumer old_installed_sdk_consumer.cpp)
target_compile_features(sample-region-old-sdk-consumer PRIVATE cxx_std_23)
target_link_libraries(sample-region-old-sdk-consumer PRIVATE pulp::host pulp::native-components)
