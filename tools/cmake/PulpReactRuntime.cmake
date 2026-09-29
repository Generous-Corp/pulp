# pulp_check_vendored_react_runtime(<bundle> [STRICT] [FINGERPRINT <json>])
#
# Configure-time warning for an app that vendors a compiled @pulp/react runtime
# (the `runtime.js` a materialized or JSX import emits, usually checked in and
# embedded as bytes). Nothing rebuilds that bundle when the SDK improves, so an
# app can build against a current SDK while running a runtime that predates
# its fixes. This compares the bundle against the SDK's
# runtime-fingerprint.json and names each missing fix and the refresh command.
#
# A fix counts as present when the bundle's `/* @pulp/react runtime revision N */`
# banner is at or past the fix's revision, or when the fix's signature
# identifier appears in the bundle (which judges bundles emitted before the
# banner existed). It warns by default; STRICT makes a stale bundle a configure
# error. The bundle and fingerprint are configure dependencies, so refreshing
# either re-runs the check. Same rules as
# tools/import-validation/check_vendored_runtime.py.

set(_PULP_REACT_RUNTIME_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_pulp_react_runtime_fingerprint_path out_var)
    # Installed SDK: shipped beside this module. Source tree: the package's own.
    foreach(_candidate
            "${_PULP_REACT_RUNTIME_MODULE_DIR}/pulp-react-runtime-fingerprint.json"
            "${_PULP_REACT_RUNTIME_MODULE_DIR}/../../packages/pulp-react/runtime-fingerprint.json")
        if(EXISTS "${_candidate}")
            get_filename_component(_candidate "${_candidate}" ABSOLUTE)
            set(${out_var} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(pulp_check_vendored_react_runtime bundle)
    cmake_parse_arguments(P "STRICT" "FINGERPRINT" "" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_check_vendored_react_runtime: unexpected arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()

    get_filename_component(_bundle "${bundle}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_bundle}")
        message(FATAL_ERROR
            "pulp_check_vendored_react_runtime: no bundle at ${_bundle}")
    endif()

    set(_fingerprint "${P_FINGERPRINT}")
    if(NOT _fingerprint)
        _pulp_react_runtime_fingerprint_path(_fingerprint)
    endif()
    if(NOT _fingerprint OR NOT EXISTS "${_fingerprint}")
        # Said out loud: a check that could not run must not read as a pass.
        message(WARNING
            "pulp_check_vendored_react_runtime: this SDK ships no @pulp/react "
            "runtime fingerprint, so ${_bundle} was NOT checked for staleness")
        return()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_bundle}" "${_fingerprint}")

    file(READ "${_fingerprint}" _manifest)
    string(JSON _sdk_revision GET "${_manifest}" revision)
    string(JSON _fix_count LENGTH "${_manifest}" fixes)

    file(READ "${_bundle}" _text)
    set(_bundle_revision 0)
    set(_stamp "no revision banner")
    if(_text MATCHES "@pulp/react runtime revision ([0-9]+)")
        set(_bundle_revision "${CMAKE_MATCH_1}")
        set(_stamp "revision ${_bundle_revision}")
    endif()

    set(_missing "")
    if(_fix_count GREATER 0)
        math(EXPR _last "${_fix_count} - 1")
        foreach(_i RANGE ${_last})
            string(JSON _fix_revision GET "${_manifest}" fixes ${_i} revision)
            if(_bundle_revision GREATER_EQUAL _fix_revision)
                continue()
            endif()
            string(JSON _signature ERROR_VARIABLE _no_signature
                GET "${_manifest}" fixes ${_i} signature)
            if(NOT _no_signature AND NOT _signature STREQUAL "")
                string(FIND "${_text}" "${_signature}" _at)
                if(NOT _at EQUAL -1)
                    continue()
                endif()
            endif()
            string(JSON _id GET "${_manifest}" fixes ${_i} id)
            string(JSON _summary GET "${_manifest}" fixes ${_i} summary)
            string(APPEND _missing "\n  - [${_id}] ${_summary}")
        endforeach()
    endif()

    if(_missing STREQUAL "")
        message(STATUS
            "Vendored @pulp/react runtime is current (${_stamp}; SDK revision "
            "${_sdk_revision}): ${_bundle}")
        return()
    endif()

    set(_severity WARNING)
    if(P_STRICT)
        set(_severity FATAL_ERROR)
    endif()
    message(${_severity}
        "Vendored @pulp/react runtime is stale: ${_bundle} (${_stamp}) predates "
        "SDK revision ${_sdk_revision} and is missing:${_missing}\n"
        "Refresh it by re-running the import transform that produced it "
        "(tools/import-design/jsx-runtime/materialized-runtime-transform.mjs or "
        "jsx-transform.mjs, or `pulp import-design`) against this SDK, then "
        "commit the regenerated bundle.")
endfunction()
