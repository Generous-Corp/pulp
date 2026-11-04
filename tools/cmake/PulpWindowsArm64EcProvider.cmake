# Fail-closed validation for Windows ARM64EC/ARM64X Skia/Dawn providers.
#
# A provider manifest is not sufficient: stale or mis-built .lib archives can
# retain only ARM64 members while claiming arm64ec. The Python helper parses
# the actual COFF archive members before Pulp creates imported link targets.

include_guard(GLOBAL)

function(pulp_windows_validate_arm64ec_provider)
    if(NOT WIN32 AND NOT PULP_WINDOWS_ARM64EC_PROVIDER_CONTRACT_TEST)
        return()
    endif()

    if(NOT PULP_WINDOWS_ARM64EC_PROVIDER_TARGET)
        set(PULP_WINDOWS_ARM64EC_PROVIDER_TARGET "arm64ec")
    endif()
    string(TOLOWER "${PULP_WINDOWS_ARM64EC_PROVIDER_TARGET}" _target)
    if(NOT _target MATCHES "^(arm64ec|arm64x)$")
        message(FATAL_ERROR
            "Pulp: unsupported ARM64EC provider target '${_target}'")
    endif()

    if(NOT PULP_WINDOWS_ARM64EC_PROVIDER_VALIDATOR)
        # Installed SDKs place helper scripts beside the CMake modules. The
        # source tree keeps Windows tools under tools/windows, so support both
        # layouts without silently falling back to an unvalidated provider.
        set(_pulp_validator_installed
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/validate_arm64ec_provider.py")
        set(_pulp_validator_source
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../windows/validate_arm64ec_provider.py")
        if(EXISTS "${_pulp_validator_installed}")
            set(PULP_WINDOWS_ARM64EC_PROVIDER_VALIDATOR
                "${_pulp_validator_installed}")
        else()
            set(PULP_WINDOWS_ARM64EC_PROVIDER_VALIDATOR
                "${_pulp_validator_source}")
        endif()
        unset(_pulp_validator_installed)
        unset(_pulp_validator_source)
    endif()
    if(NOT EXISTS "${PULP_WINDOWS_ARM64EC_PROVIDER_VALIDATOR}")
        message(FATAL_ERROR
            "Pulp: ARM64EC provider validator is missing at "
            "'${PULP_WINDOWS_ARM64EC_PROVIDER_VALIDATOR}'")
    endif()

    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter QUIET)
    endif()
    if(NOT Python3_Interpreter_FOUND AND NOT Python3_EXECUTABLE)
        message(FATAL_ERROR
            "Pulp: ARM64EC provider validation requires Python 3")
    endif()

    if(NOT PULP_WINDOWS_ARM64EC_PROVIDER_METADATA)
        set(PULP_WINDOWS_ARM64EC_PROVIDER_METADATA
            "${SKIA_DIR}/build/win-gpu/msvc-toolchain.json")
    endif()
    if(NOT EXISTS "${PULP_WINDOWS_ARM64EC_PROVIDER_METADATA}")
        message(FATAL_ERROR
            "Pulp: ARM64EC/ARM64X provider metadata is missing at "
            "'${PULP_WINDOWS_ARM64EC_PROVIDER_METADATA}'; refusing an "
            "unproven static provider")
    endif()

    set(_command "${Python3_EXECUTABLE}" "${PULP_WINDOWS_ARM64EC_PROVIDER_VALIDATOR}"
        --target "${_target}"
        --metadata "${PULP_WINDOWS_ARM64EC_PROVIDER_METADATA}")
    foreach(_archive IN LISTS PULP_WINDOWS_ARM64EC_PROVIDER_ARCHIVES)
        if(NOT EXISTS "${_archive}")
            message(FATAL_ERROR
                "Pulp: ARM64EC provider archive is missing: '${_archive}'")
        endif()
        list(APPEND _command --archive "${_archive}")
    endforeach()
    execute_process(
        COMMAND ${_command}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "Pulp: ARM64EC provider archive validation failed.\n"
            "${_output}\n${_error}")
    endif()
    message(STATUS "Pulp: ${_output}")
endfunction()
