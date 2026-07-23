cmake_minimum_required(VERSION 3.24)
project(PulpTimelineSdkConsumer VERSION 0.1.0 LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS timebase timeline playback)

foreach(_component timebase timeline playback)
    if(NOT Pulp_${_component}_FOUND)
        message(FATAL_ERROR "Pulp did not report required component ${_component}")
    endif()
endforeach()

add_executable(pulp-timeline-sdk-consumer main.cpp)
target_link_libraries(pulp-timeline-sdk-consumer PRIVATE
    Pulp::timebase
    Pulp::timeline
    Pulp::playback
)

# Audit the installed target graph, not a hand-maintained dependency list.
function(_pulp_collect_dependencies target)
    get_property(_visited GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE)
    if(NOT _visited)
        set(_visited "")
    endif()
    list(FIND _visited "${target}" _already_visited)
    if(NOT _already_visited EQUAL -1)
        return()
    endif()
    list(APPEND _visited "${target}")
    set_property(GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE "${_visited}")

    get_target_property(_links "${target}" INTERFACE_LINK_LIBRARIES)
    if(NOT _links)
        return()
    endif()
    string(REGEX MATCHALL "Pulp::[A-Za-z0-9_-]+" _pulp_links "${_links}")
    foreach(_dependency IN LISTS _pulp_links)
        if(TARGET "${_dependency}")
            _pulp_collect_dependencies("${_dependency}")
        endif()
    endforeach()
endfunction()

set_property(GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE "")
foreach(_root Pulp::timebase Pulp::timeline Pulp::playback)
    _pulp_collect_dependencies("${_root}")
endforeach()
get_property(_actual_closure GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE)
list(REMOVE_DUPLICATES _actual_closure)
list(SORT _actual_closure)

set(_expected_engine_closure
    Pulp::audio
    Pulp::events
    Pulp::midi
    Pulp::platform
    Pulp::playback
    Pulp::runtime
    Pulp::signal
    Pulp::state
    Pulp::timebase
    Pulp::timeline
)
set(_expected_support_closure
    Pulp::everest
    Pulp::hwy
    Pulp::mbedcrypto
    Pulp::mbedtls
    Pulp::mbedx509
    Pulp::p256m
)
set(_expected_closure ${_expected_engine_closure} ${_expected_support_closure})
list(SORT _expected_closure)
if(NOT _actual_closure STREQUAL _expected_closure)
    message(FATAL_ERROR
        "Timeline SDK closure drifted.\n"
        "Expected: ${_expected_closure}\n"
        "Actual:   ${_actual_closure}")
endif()

foreach(_forbidden
        Pulp::canvas Pulp::format Pulp::gpu-audio Pulp::graph Pulp::host
        Pulp::render Pulp::standalone Pulp::view Pulp::view-core)
    if(_forbidden IN_LIST _actual_closure)
        message(FATAL_ERROR
            "Timeline SDK unexpectedly pulled ${_forbidden} into its link closure")
    endif()
endforeach()
