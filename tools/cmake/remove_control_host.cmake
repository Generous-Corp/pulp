cmake_minimum_required(VERSION 3.24)
if(DEFINED PULP_CONTROL_HOST_ID)
    string(LENGTH "${PULP_CONTROL_HOST_ID}" _host_id_length)
endif()
if(NOT DEFINED PULP_CONTROL_HOST_ID OR
   NOT PULP_CONTROL_HOST_ID MATCHES "^[a-z0-9][a-z0-9_-]*$" OR
   _host_id_length GREATER 128 OR NOT DEFINED PULP_CONTROL_HOST_ROOT OR
   NOT IS_ABSOLUTE "${PULP_CONTROL_HOST_ROOT}")
    message(FATAL_ERROR "remove_control_host.cmake requires a valid id and absolute root")
endif()
set(_entry "${PULP_CONTROL_HOST_ROOT}/${PULP_CONTROL_HOST_ID}")
set(_active "${_entry}/active")
if(IS_SYMLINK "${PULP_CONTROL_HOST_ROOT}" OR IS_SYMLINK "${_entry}" OR
   IS_SYMLINK "${_active}")
    message(FATAL_ERROR "control host catalog paths must not be symlinks")
endif()
if(NOT EXISTS "${_active}")
    return()
endif()
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef _nonce)
file(RENAME "${_active}" "${_entry}/.active.removed-${_nonce}"
    RESULT _remove_result)
if(NOT _remove_result STREQUAL "0")
    message(FATAL_ERROR "could not atomically remove control host selection: ${_remove_result}")
endif()
file(REMOVE "${_entry}/.active.removed-${_nonce}")
