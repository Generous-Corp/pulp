if(NOT DEFINED CLI OR NOT DEFINED WORKING_DIR OR NOT DEFINED EXPECTED_EXIT OR NOT DEFINED EXPECTED_REGEX)
    message(FATAL_ERROR "check_cli_host_error.cmake requires CLI, WORKING_DIR, EXPECTED_EXIT, and EXPECTED_REGEX")
endif()

set(host_args host)

if(DEFINED HELP_FLAG)
    list(APPEND host_args "${HELP_FLAG}")
endif()

if(DEFINED PLUGIN_PATH)
    list(APPEND host_args "${PLUGIN_PATH}")
endif()

if(DEFINED UNIQUE_ID)
    list(APPEND host_args --id "${UNIQUE_ID}")
endif()

if(DEFINED FORMAT)
    if(DEFINED FORMAT_FLAG)
        set(_format_flag "${FORMAT_FLAG}")
    else()
        set(_format_flag "--format")
    endif()
    list(APPEND host_args "${_format_flag}" "${FORMAT}")
endif()

execute_process(
    COMMAND "${CLI}" ${host_args}
    WORKING_DIRECTORY "${WORKING_DIR}"
    RESULT_VARIABLE cli_result
    OUTPUT_VARIABLE cli_stdout
    ERROR_VARIABLE cli_stderr
)

set(cli_output "${cli_stdout}${cli_stderr}")

if(NOT cli_result EQUAL EXPECTED_EXIT)
    message(FATAL_ERROR "expected exit ${EXPECTED_EXIT}, got ${cli_result}\n${cli_output}")
endif()

if(NOT cli_output MATCHES "${EXPECTED_REGEX}")
    message(FATAL_ERROR "expected output to match '${EXPECTED_REGEX}'\n${cli_output}")
endif()

message("verified CLI host contract")
