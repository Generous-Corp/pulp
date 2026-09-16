cmake_minimum_required(VERSION 3.25)
project(pulp_sample_region_old_sdk_consumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED)
add_executable(sample-region-old-sdk-consumer ../old_installed_sdk_consumer.cpp)
target_compile_features(sample-region-old-sdk-consumer PRIVATE cxx_std_23)
target_link_libraries(sample-region-old-sdk-consumer PRIVATE Pulp::host Pulp::native-components)
