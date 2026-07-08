# PulpPluginMetadata.cmake - shared plugin metadata resolution helpers.
#
# Keep package/host-discovery decisions here instead of duplicating them across
# AU v2, AUv3, iOS AUv3, and future format helpers. These functions are
# intentionally CMake-only so installed SDK consumers get the same behavior as
# source-tree builds through find_package(Pulp).

function(_pulp_metadata_require_fourcc context label value)
    string(LENGTH "${value}" _pulp_fourcc_len)
    if(NOT _pulp_fourcc_len EQUAL 4)
        message(FATAL_ERROR
            "pulp: ${context}: ${label} must be exactly 4 characters "
            "(got '${value}', length ${_pulp_fourcc_len}).")
    endif()
endfunction()

function(_pulp_metadata_parse_au_version out_var context version)
    if(NOT "${version}" MATCHES "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?($|[-+])")
        message(FATAL_ERROR
            "pulp: ${context}: VERSION must be numeric major[.minor[.patch]] "
            "with an optional '-' or '+' suffix "
            "for Audio Unit metadata (got '${version}').")
    endif()

    set(_pulp_au_ver_major "${CMAKE_MATCH_1}")
    if(CMAKE_MATCH_3)
        set(_pulp_au_ver_minor "${CMAKE_MATCH_3}")
    else()
        set(_pulp_au_ver_minor 0)
    endif()
    if(CMAKE_MATCH_5)
        set(_pulp_au_ver_patch "${CMAKE_MATCH_5}")
    else()
        set(_pulp_au_ver_patch 0)
    endif()

    math(EXPR _pulp_au_version_int
        "${_pulp_au_ver_major} * 65536 + ${_pulp_au_ver_minor} * 256 + ${_pulp_au_ver_patch}")
    set(${out_var} "${_pulp_au_version_int}" PARENT_SCOPE)
endfunction()

function(_pulp_metadata_resolve_au out_prefix context category accepts_midi version plugin_code manufacturer_code)
    _pulp_metadata_require_fourcc("${context}" "PLUGIN_CODE/SUBTYPE_CODE" "${plugin_code}")
    _pulp_metadata_require_fourcc("${context}" "MANUFACTURER_CODE" "${manufacturer_code}")

    if(NOT "${category}" STREQUAL "Effect"
            AND NOT "${category}" STREQUAL "Instrument"
            AND NOT "${category}" STREQUAL "MidiEffect")
        message(FATAL_ERROR
            "pulp: ${context}: CATEGORY must be Effect, Instrument, or MidiEffect "
            "(got '${category}').")
    endif()

    if("${category}" STREQUAL "Instrument")
        set(_pulp_au_type "aumu")
        set(_pulp_au_tag "Synthesizer")
    elseif("${category}" STREQUAL "MidiEffect")
        set(_pulp_au_type "aumi")
        set(_pulp_au_tag "MIDI")
    elseif("${accepts_midi}")
        # AU hosts only route MIDI to effects packaged as MusicEffect.
        set(_pulp_au_type "aumf")
        set(_pulp_au_tag "Effects")
    else()
        set(_pulp_au_type "aufx")
        set(_pulp_au_tag "Effects")
    endif()

    _pulp_metadata_parse_au_version(_pulp_au_version_int "${context}" "${version}")

    set(${out_prefix}_TYPE "${_pulp_au_type}" PARENT_SCOPE)
    set(${out_prefix}_TAG "${_pulp_au_tag}" PARENT_SCOPE)
    set(${out_prefix}_VERSION_INT "${_pulp_au_version_int}" PARENT_SCOPE)
    set(${out_prefix}_PLUGIN_CODE "${plugin_code}" PARENT_SCOPE)
    set(${out_prefix}_MANUFACTURER_CODE "${manufacturer_code}" PARENT_SCOPE)
endfunction()
