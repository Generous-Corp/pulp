# Refuse to configure a Pulp checkout that lives in a temporary directory.
#
# Configure is the one step every build path shares: `pulp build`, the C++
# delegate, Shipyard's local lane, and a raw `cmake -S . -B build` alike. The
# policy itself lives in tools/ci/checkout_location_guard.py so governed-build.sh
# applies the identical rule to trees that were configured before it existed.
#
# Only a top-level configure of this source tree is checked: a consumer that
# add_subdirectory()s Pulp owns its own location. Windows has no /tmp
# convention worth policing, and a host without python3 cannot run the check,
# so both skip it rather than fail.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_HOST_WIN32)
    find_program(PULP_CHECKOUT_GUARD_PYTHON NAMES python3)
    mark_as_advanced(PULP_CHECKOUT_GUARD_PYTHON)
    if(PULP_CHECKOUT_GUARD_PYTHON)
        execute_process(
            COMMAND "${PULP_CHECKOUT_GUARD_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/checkout_location_guard.py"
                --context "Pulp configure"
                "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE _pulp_checkout_guard_rc
            ERROR_VARIABLE _pulp_checkout_guard_msg
            TIMEOUT 30)
        string(STRIP "${_pulp_checkout_guard_msg}" _pulp_checkout_guard_msg)
        if(_pulp_checkout_guard_rc EQUAL 3)
            message(FATAL_ERROR "${_pulp_checkout_guard_msg}")
        elseif(_pulp_checkout_guard_msg)
            message(WARNING "${_pulp_checkout_guard_msg}")
        endif()
        unset(_pulp_checkout_guard_rc)
        unset(_pulp_checkout_guard_msg)
    endif()
endif()
