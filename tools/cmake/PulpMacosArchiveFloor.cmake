# PulpMacosArchiveFloor.cmake — reject Apple prebuilts above the target floor.
#
# CMAKE_OSX_DEPLOYMENT_TARGET controls objects compiled by the consumer, but it
# cannot lower the LC_BUILD_VERSION stamped into a prebuilt static archive.  If
# an archive was accidentally built on a newer CI runner without an explicit
# deployment target, ld only warns and emits a binary that does not satisfy the
# consumer's claimed minimum OS.  Measure every selected archive before link so
# that mismatch is a configure error instead of a shipping surprise.

include_guard(GLOBAL)

function(_pulp_macos_parse_otool_floor out_var otool_output)
    string(REPLACE "\r\n" "\n" _text "${otool_output}")
    string(REPLACE "\r" "\n" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")

    set(_floor "")
    set(_field "")
    set(_remaining 0)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "LC_BUILD_VERSION")
            set(_field "minos")
            set(_remaining 6)
        elseif(_line MATCHES "LC_VERSION_MIN_(MACOSX|IPHONEOS|TVOS|WATCHOS)")
            set(_field "version")
            set(_remaining 6)
        elseif(_remaining GREATER 0)
            if(_field STREQUAL "minos" AND
                    _line MATCHES "^[ \t]*minos[ \t]+([0-9]+(\\.[0-9]+)+)")
                set(_value "${CMAKE_MATCH_1}")
                if(_floor STREQUAL "" OR _value VERSION_GREATER _floor)
                    set(_floor "${_value}")
                endif()
                set(_remaining 0)
            elseif(_field STREQUAL "version" AND
                    _line MATCHES "^[ \t]*version[ \t]+([0-9]+(\\.[0-9]+)+)")
                set(_value "${CMAKE_MATCH_1}")
                if(_floor STREQUAL "" OR _value VERSION_GREATER _floor)
                    set(_floor "${_value}")
                endif()
                set(_remaining 0)
            else()
                math(EXPR _remaining "${_remaining} - 1")
            endif()
        endif()
    endforeach()

    set(${out_var} "${_floor}" PARENT_SCOPE)
endfunction()

function(pulp_assert_macos_archive_floor)
    set(_one TARGET OTOOL)
    set(_multi ARCHIVES)
    cmake_parse_arguments(PULP_FLOOR "" "${_one}" "${_multi}" ${ARGN})

    if(NOT PULP_FLOOR_TARGET)
        message(FATAL_ERROR
            "Pulp macOS archive-floor check requires TARGET. "
            "Pin CMAKE_OSX_DEPLOYMENT_TARGET before selecting Apple prebuilts.")
    endif()
    if(NOT PULP_FLOOR_ARCHIVES)
        message(FATAL_ERROR "Pulp macOS archive-floor check requires ARCHIVES.")
    endif()

    if(PULP_FLOOR_OTOOL)
        set(_otool "${PULP_FLOOR_OTOOL}")
    else()
        find_program(_otool NAMES otool REQUIRED)
    endif()

    foreach(_archive IN LISTS PULP_FLOOR_ARCHIVES)
        if(NOT EXISTS "${_archive}")
            message(FATAL_ERROR
                "Pulp macOS archive-floor check cannot inspect missing archive: ${_archive}")
        endif()
        execute_process(
            COMMAND "${_otool}" -l "${_archive}"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _load_commands
            ERROR_VARIABLE _error)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "Pulp macOS archive-floor check could not inspect ${_archive}: ${_error}")
        endif()

        _pulp_macos_parse_otool_floor(_archive_floor "${_load_commands}")
        if(_archive_floor STREQUAL "")
            message(FATAL_ERROR
                "Pulp macOS archive-floor check found no LC_BUILD_VERSION or "
                "LC_VERSION_MIN_* record in ${_archive}.")
        endif()
        if(_archive_floor VERSION_GREATER PULP_FLOOR_TARGET)
            message(FATAL_ERROR
                "Pulp macOS archive-floor mismatch:\n"
                "  archive: ${_archive}\n"
                "  archive minimum: macOS ${_archive_floor}\n"
                "  consumer target: macOS ${PULP_FLOOR_TARGET}\n"
                "Select a prebuilt compiled at or below the consumer target. "
                "Raising only the consumer compile flag cannot lower an "
                "archive's embedded LC_BUILD_VERSION.")
        endif()
        message(STATUS
            "Pulp macOS archive floor: ${_archive_floor} <= "
            "${PULP_FLOOR_TARGET} (${_archive})")
    endforeach()
endfunction()
