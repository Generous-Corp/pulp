# Optional MIDI tuning provider helpers for installed SDK consumers.

# Directory holding optional MIDI tuning provider sources. Installed SDK
# consumers can opt into these wrappers after `pulp add` fetches the underlying
# third-party package, without requiring the SDK itself to ship those packages
# in its default `pulp-midi` archive.
if(DEFINED PULP_MIDI_SOURCE_DIR AND EXISTS "${PULP_MIDI_SOURCE_DIR}")
    set(_PULP_MIDI_SOURCE_DIR "${PULP_MIDI_SOURCE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../core/midi/src")
    set(_PULP_MIDI_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../core/midi/src")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/midi")
    set(_PULP_MIDI_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/midi")
else()
    set(_PULP_MIDI_SOURCE_DIR "")
endif()

function(_pulp_target_has_compile_definition out_var target definition)
    set(_found FALSE)
    if(TARGET "${target}")
        foreach(_prop INTERFACE_COMPILE_DEFINITIONS COMPILE_DEFINITIONS)
            get_target_property(_defs "${target}" "${_prop}")
            if(_defs)
                foreach(_def IN LISTS _defs)
                    if(_def STREQUAL "${definition}" OR _def STREQUAL "${definition}=1")
                        set(_found TRUE)
                    endif()
                endforeach()
            endif()
        endforeach()
    endif()
    set(${out_var} ${_found} PARENT_SCOPE)
endfunction()

function(_pulp_enable_one_midi_tuning_provider target provider)
    string(TOUPPER "${provider}" _provider)
    if(_provider STREQUAL "MTS" OR _provider STREQUAL "MTS_ESP" OR _provider STREQUAL "MTS-ESP")
        set(_provider_key "MTS_ESP")
        set(_definition "PULP_HAS_MTS_ESP")
        set(_source "mts_esp_tuning.cpp")
        set(_package_target "mts_esp_client")
        set(_package_hint "pulp add mts-esp")
    elseif(_provider STREQUAL "SCALA" OR _provider STREQUAL "SCALA_TUNING" OR _provider STREQUAL "SCL_KBM")
        set(_provider_key "SCALA_TUNING")
        set(_definition "PULP_HAS_SCALA_TUNING")
        set(_source "scala_tuning.cpp")
        set(_package_target "sst::tuning-library")
        set(_package_hint "pulp add sst-tuning-library")
    else()
        message(FATAL_ERROR
            "pulp_enable_midi_tuning_provider(${target} ...): unknown provider "
            "'${provider}'. Expected MTS_ESP or SCALA.")
    endif()

    get_target_property(_already "${target}" "PULP_MIDI_TUNING_${_provider_key}_ENABLED")
    if(_already)
        return()
    endif()

    if(_PULP_MIDI_TARGET)
        target_link_libraries(${target} PRIVATE "${_PULP_MIDI_TARGET}")
        _pulp_target_has_compile_definition(
            _sdk_has_provider "${_PULP_MIDI_TARGET}" "${_definition}")
    else()
        set(_sdk_has_provider FALSE)
    endif()

    target_compile_definitions(${target} PRIVATE "${_definition}=1")

    if(_sdk_has_provider)
        set_target_properties(${target} PROPERTIES
            "PULP_MIDI_TUNING_${_provider_key}_ENABLED" TRUE)
        return()
    endif()

    if(NOT TARGET "${_package_target}")
        message(FATAL_ERROR
            "pulp_enable_midi_tuning_provider(${target} ${provider}) requires "
            "CMake target ${_package_target}. Run `${_package_hint}` and include "
            "cmake/pulp-packages.cmake before calling this helper, or rebuild "
            "Pulp with the matching provider option enabled.")
    endif()

    set(_provider_source "${_PULP_MIDI_SOURCE_DIR}/${_source}")
    if(NOT _PULP_MIDI_SOURCE_DIR OR NOT EXISTS "${_provider_source}")
        message(FATAL_ERROR
            "pulp_enable_midi_tuning_provider(${target} ${provider}) could not "
            "find ${_source}. Installed SDKs must ship optional MIDI provider "
            "sources under src/pulp/midi; source builds should have core/midi/src. "
            "Reinstall or upgrade the Pulp SDK.")
    endif()

    target_sources(${target} PRIVATE "${_provider_source}")
    target_link_libraries(${target} PRIVATE "${_package_target}")
    set_target_properties(${target} PROPERTIES
        "PULP_MIDI_TUNING_${_provider_key}_ENABLED" TRUE)
endfunction()

function(pulp_enable_midi_tuning_provider target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "pulp_enable_midi_tuning_provider(${target} ...): target does not exist. "
            "Call this after add_library(), add_executable(), or pulp_add_plugin().")
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR
            "pulp_enable_midi_tuning_provider(${target} ...): pass at least one "
            "provider: MTS_ESP or SCALA.")
    endif()

    foreach(_provider IN LISTS ARGN)
        _pulp_enable_one_midi_tuning_provider("${target}" "${_provider}")
    endforeach()
endfunction()
