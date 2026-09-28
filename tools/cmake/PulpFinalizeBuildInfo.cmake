# PulpFinalizeBuildInfo.cmake — build-time step that turns a configure-time
# build-info template into the final JSON record.
#
# Run as a script (`cmake -P`). Resolved at BUILD time rather than configure
# time because the source commit is the one fact a configure-time value gets
# wrong: committing and rebuilding without reconfiguring would otherwise stamp
# the previous commit into the product.
#
# Inputs (-D):
#   PULP_BUILD_INFO_TEMPLATE   template JSON containing the placeholder tokens
#   PULP_BUILD_INFO_OUTPUT     final JSON path (written only when it changes)
#   PULP_BUILD_INFO_GIT_DIR    directory whose git checkout identifies the source
#   PULP_BUILD_INFO_GIT_SHA    explicit commit; skips git when non-empty
#   PULP_BUILD_INFO_GIT_DIRTY  explicit dirty flag (TRUE/FALSE); skips git when set
#   PULP_BUILD_INFO_EMBED      optional JSON file substituted for the embed token;
#                              a missing file substitutes `null`
#   PULP_BUILD_INFO_GIT        git executable (optional; looked up when empty)
#
# Tokens: "__PULP_GIT_SHA__" (a JSON string, quotes included),
# __PULP_GIT_DIRTY__ (a bare JSON boolean or null) and __PULP_EMBED__ (a bare
# JSON value).

cmake_minimum_required(VERSION 3.24)

foreach(_required PULP_BUILD_INFO_TEMPLATE PULP_BUILD_INFO_OUTPUT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "PulpFinalizeBuildInfo: ${_required} is required")
    endif()
endforeach()
if(NOT EXISTS "${PULP_BUILD_INFO_TEMPLATE}")
    message(FATAL_ERROR
        "PulpFinalizeBuildInfo: template not found: ${PULP_BUILD_INFO_TEMPLATE}")
endif()

set(_sha "")
set(_dirty "")
if(NOT "${PULP_BUILD_INFO_GIT_SHA}" STREQUAL "")
    set(_sha "${PULP_BUILD_INFO_GIT_SHA}")
endif()
if(NOT "${PULP_BUILD_INFO_GIT_DIRTY}" STREQUAL "")
    if(PULP_BUILD_INFO_GIT_DIRTY)
        set(_dirty "true")
    else()
        set(_dirty "false")
    endif()
endif()

if((_sha STREQUAL "" OR _dirty STREQUAL "") AND
   NOT "${PULP_BUILD_INFO_GIT_DIR}" STREQUAL "" AND
   IS_DIRECTORY "${PULP_BUILD_INFO_GIT_DIR}")
    set(_git "${PULP_BUILD_INFO_GIT}")
    if(_git STREQUAL "" OR NOT EXISTS "${_git}")
        find_program(_pulp_build_info_found_git NAMES git)
        set(_git "${_pulp_build_info_found_git}")
    endif()
    if(_git)
        execute_process(
            COMMAND "${_git}" -C "${PULP_BUILD_INFO_GIT_DIR}" rev-parse --verify HEAD
            RESULT_VARIABLE _rev_result
            OUTPUT_VARIABLE _rev
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_rev_result EQUAL 0 AND _rev MATCHES "^[0-9a-f]+$")
            if(_sha STREQUAL "")
                set(_sha "${_rev}")
            endif()
            if(_dirty STREQUAL "")
                execute_process(
                    COMMAND "${_git}" -C "${PULP_BUILD_INFO_GIT_DIR}"
                        status --porcelain --untracked-files=no
                        --ignore-submodules=untracked
                    RESULT_VARIABLE _status_result
                    OUTPUT_VARIABLE _status
                    ERROR_QUIET)
                if(_status_result EQUAL 0)
                    if(_status STREQUAL "")
                        set(_dirty "false")
                    else()
                        set(_dirty "true")
                    endif()
                endif()
            endif()
        endif()
    endif()
endif()

if(_sha STREQUAL "")
    set(_sha "unknown")
endif()
if(_dirty STREQUAL "")
    set(_dirty "null")
endif()
# The commit is emitted as a JSON string; refuse anything that would need
# escaping rather than emitting malformed JSON.
if(NOT _sha MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR
        "PulpFinalizeBuildInfo: source commit '${_sha}' contains characters "
        "outside [A-Za-z0-9._+-]")
endif()

set(_embed "null")
if(NOT "${PULP_BUILD_INFO_EMBED}" STREQUAL "" AND EXISTS "${PULP_BUILD_INFO_EMBED}")
    file(READ "${PULP_BUILD_INFO_EMBED}" _embed)
    string(STRIP "${_embed}" _embed)
    string(JSON _embed_type ERROR_VARIABLE _embed_error TYPE "${_embed}")
    if(_embed_error OR NOT _embed_type STREQUAL "OBJECT")
        message(FATAL_ERROR
            "PulpFinalizeBuildInfo: ${PULP_BUILD_INFO_EMBED} is not a JSON object")
    endif()
    # Re-indent the embedded object one level so the record stays readable.
    string(REPLACE "\n" "\n  " _embed "${_embed}")
endif()

file(READ "${PULP_BUILD_INFO_TEMPLATE}" _content)
string(REPLACE "\"__PULP_GIT_SHA__\"" "\"${_sha}\"" _content "${_content}")
string(REPLACE "__PULP_GIT_DIRTY__" "${_dirty}" _content "${_content}")
string(REPLACE "__PULP_EMBED__" "${_embed}" _content "${_content}")

string(JSON _schema ERROR_VARIABLE _parse_error GET "${_content}" schema)
if(_parse_error)
    message(FATAL_ERROR
        "PulpFinalizeBuildInfo: produced invalid JSON for "
        "${PULP_BUILD_INFO_OUTPUT}: ${_parse_error}")
endif()

set(_existing "")
if(EXISTS "${PULP_BUILD_INFO_OUTPUT}")
    file(READ "${PULP_BUILD_INFO_OUTPUT}" _existing)
endif()
if(NOT _existing STREQUAL _content)
    get_filename_component(_output_dir "${PULP_BUILD_INFO_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_output_dir}")
    file(WRITE "${PULP_BUILD_INFO_OUTPUT}" "${_content}")
endif()
