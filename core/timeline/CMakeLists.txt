get_filename_component(_PULP_TIMELINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include(PulpTimelineSources.cmake)
pulp_resolve_timeline_sources("${_PULP_TIMELINE_ROOT}" _PULP_TIMELINE_SOURCES)
# Browser timeline builds consume only the portable list above. Native
# crash-consistent storage remains a separate source set so filesystem code
# cannot enter WAM/WebCLAP by list drift.
pulp_resolve_timeline_native_sources(
    "${_PULP_TIMELINE_ROOT}" _PULP_TIMELINE_NATIVE_SOURCES)

add_library(pulp-timeline
    ${_PULP_TIMELINE_SOURCES}
    ${_PULP_TIMELINE_NATIVE_SOURCES})
add_library(pulp::timeline ALIAS pulp-timeline)

target_compile_features(pulp-timeline PUBLIC cxx_std_20)

target_include_directories(pulp-timeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timeline
    PUBLIC
        pulp::runtime
        pulp::timebase
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timeline PRIVATE -fno-exceptions -fno-rtti)
endif()

# Standalone generator for the schema-drift gate: emits the canonical
# JSON-Schema projection of the built-in registry (see tools/schema_drift_check.py).
add_executable(pulp-timeline-schema-emit tools/schema_emit_main.cpp)
target_link_libraries(pulp-timeline-schema-emit PRIVATE pulp::timeline)
