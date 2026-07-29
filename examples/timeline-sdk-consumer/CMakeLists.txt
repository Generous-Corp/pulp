cmake_minimum_required(VERSION 3.24)
project(PulpTimelineSdkConsumer VERSION 0.1.0 LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS timebase timeline playback dawproject-import dawproject-export smf-interop smf-interchange)

foreach(_component timebase timeline playback dawproject-import dawproject-export smf-interop smf-interchange)
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

add_executable(pulp-timeline-cookbook-consumer cookbook.cpp)
target_link_libraries(pulp-timeline-cookbook-consumer PRIVATE
    Pulp::timebase
    Pulp::timeline
    Pulp::playback
)

add_executable(pulp-dawproject-import-sdk-consumer dawproject_import.cpp)
target_link_libraries(pulp-dawproject-import-sdk-consumer PRIVATE
    Pulp::dawproject-import
)

add_executable(pulp-dawproject-export-sdk-consumer dawproject_export.cpp)
target_link_libraries(pulp-dawproject-export-sdk-consumer PRIVATE
    Pulp::dawproject-export
)

add_executable(pulp-smf-interchange-sdk-consumer smf_interchange.cpp)
target_link_libraries(pulp-smf-interchange-sdk-consumer PRIVATE
    Pulp::smf-interchange
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
    # Arrives transitively: playback -> audio -> sample-bank-manifest. pulp-audio
    # PUBLIC-links it, so every consumer of the engine closure links it too.
    Pulp::sample-bank-manifest
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

set_property(GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE "")
_pulp_collect_dependencies(Pulp::smf-interop)
get_property(_actual_smf_codec_closure GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE)
list(REMOVE_DUPLICATES _actual_smf_codec_closure)
list(SORT _actual_smf_codec_closure)

set(_expected_smf_codec_closure
    Pulp::everest
    Pulp::hwy
    Pulp::mbedcrypto
    Pulp::mbedtls
    Pulp::mbedx509
    Pulp::p256m
    Pulp::platform
    Pulp::runtime
    Pulp::smf-interop
    Pulp::timebase
    Pulp::timeline
)
list(SORT _expected_smf_codec_closure)
if(NOT _actual_smf_codec_closure STREQUAL _expected_smf_codec_closure)
    message(FATAL_ERROR
        "SMF codec SDK closure drifted.\n"
        "Expected: ${_expected_smf_codec_closure}\n"
        "Actual:   ${_actual_smf_codec_closure}")
endif()

set_property(GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE "")
_pulp_collect_dependencies(Pulp::smf-interchange)
get_property(_actual_smf_adapter_closure GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE)
list(REMOVE_DUPLICATES _actual_smf_adapter_closure)
list(SORT _actual_smf_adapter_closure)
set(_expected_smf_adapter_closure
    ${_expected_smf_codec_closure}
    Pulp::interchange
    Pulp::smf-interchange
)
list(REMOVE_DUPLICATES _expected_smf_adapter_closure)
list(SORT _expected_smf_adapter_closure)
if(NOT _actual_smf_adapter_closure STREQUAL _expected_smf_adapter_closure)
    message(FATAL_ERROR
        "SMF interchange SDK closure drifted.\n"
        "Expected: ${_expected_smf_adapter_closure}\n"
        "Actual:   ${_actual_smf_adapter_closure}")
endif()

set_property(GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE "")
_pulp_collect_dependencies(Pulp::dawproject-export)
get_property(_actual_daw_export_closure GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE)
list(REMOVE_DUPLICATES _actual_daw_export_closure)
list(SORT _actual_daw_export_closure)
set(_expected_daw_export_closure
    Pulp::dawproject-export
    Pulp::everest
    Pulp::hwy
    Pulp::interchange
    Pulp::mbedcrypto
    Pulp::mbedtls
    Pulp::mbedx509
    Pulp::p256m
    Pulp::platform
    Pulp::runtime
    Pulp::timebase
    Pulp::timeline
)
list(SORT _expected_daw_export_closure)
if(NOT _actual_daw_export_closure STREQUAL _expected_daw_export_closure)
    message(FATAL_ERROR
        "DAWproject exporter SDK closure drifted.\n"
        "Expected: ${_expected_daw_export_closure}\n"
        "Actual:   ${_actual_daw_export_closure}")
endif()

set_property(GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE "")
_pulp_collect_dependencies(Pulp::dawproject-import)
get_property(_actual_importer_closure GLOBAL PROPERTY PULP_TIMELINE_SDK_CLOSURE)
list(REMOVE_DUPLICATES _actual_importer_closure)
list(SORT _actual_importer_closure)

set(_expected_importer_closure
    Pulp::audio
    Pulp::sample-bank-manifest
    Pulp::dawproject-import
    Pulp::events
    Pulp::everest
    Pulp::hwy
    # The importer's refusal sites derive from the interchange capability table,
    # so interchange is part of its installed closure, not an internal detail.
    Pulp::interchange
    Pulp::mbedcrypto
    Pulp::mbedtls
    Pulp::mbedx509
    Pulp::midi
    Pulp::p256m
    Pulp::platform
    Pulp::runtime
    Pulp::signal
    Pulp::state
    Pulp::timebase
    Pulp::timeline
)
list(REMOVE_DUPLICATES _expected_importer_closure)
list(SORT _expected_importer_closure)
if(NOT _actual_importer_closure STREQUAL _expected_importer_closure)
    message(FATAL_ERROR
        "DAWproject importer SDK closure drifted.\n"
        "Expected: ${_expected_importer_closure}\n"
        "Actual:   ${_actual_importer_closure}")
endif()

foreach(_forbidden
        Pulp::canvas Pulp::format Pulp::gpu-audio Pulp::graph Pulp::host
        Pulp::playback Pulp::render Pulp::sequence Pulp::standalone
        Pulp::view Pulp::view-core)
    if(_forbidden IN_LIST _actual_importer_closure)
        message(FATAL_ERROR
            "DAWproject importer unexpectedly pulled ${_forbidden} into its link closure")
    endif()
endforeach()
