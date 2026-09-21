cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SampleRegionAllpass LANGUAGES C CXX)
    find_package(Pulp CONFIG REQUIRED)
    set(_allpass_pulp_namespace Pulp)
else()
    set(_allpass_pulp_namespace pulp)
    include("${PULP_ROOT_DIR}/tools/cmake/PulpUtils.cmake")
endif()

if(NOT TARGET sample-region-allpass-core)
    add_library(sample-region-allpass-core STATIC
        allpass_graph.cpp allpass_processor.cpp)
    target_compile_features(sample-region-allpass-core PUBLIC cxx_std_20)
    target_include_directories(sample-region-allpass-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(sample-region-allpass-core PUBLIC
        ${_allpass_pulp_namespace}::host ${_allpass_pulp_namespace}::format)
    set_target_properties(sample-region-allpass-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_executable(sample-region-allpass-consumer installed_consumer.cpp)
    target_link_libraries(sample-region-allpass-consumer PRIVATE
        sample-region-allpass-core ${_allpass_pulp_namespace}::audio-analysis)
endif()

# Control composition is optional; the DSP and installed consumer need no broker.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/control_targets.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/control_targets.cmake")
endif()


# Native bundles use the same Processor when their format entry points are present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp")
    pulp_add_plugin(SampleRegionAllpass
        FORMATS VST3 AU CLAP
        SOURCES allpass_graph.cpp allpass_processor.cpp
        PLUGIN_NAME "Sample Region Allpass"
        BUNDLE_ID "com.pulp.sample-region-allpass"
        MANUFACTURER "Pulp"
        VERSION "1.0.0"
        CATEGORY Effect
        PLUGIN_CODE "SrAp"
        MANUFACTURER_CODE "Pulp")
    target_link_libraries(SampleRegionAllpass_Core PUBLIC ${_allpass_pulp_namespace}::host)
endif()
