if(NOT PULP_SOURCE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and WORK_DIR are required")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/module.so" "fixture")
file(WRITE "${WORK_DIR}/readelf" "#!/bin/sh\nprintf ' (NULL)\\n'\n")
file(WRITE "${WORK_DIR}/ldd-ok" "#!/bin/sh\nprintf ' libc.so.6 => /lib/libc.so.6\\n'\nexit 0\n")
file(WRITE "${WORK_DIR}/ldd-fail" "#!/bin/sh\nprintf ' libc.so.6 => /lib/libc.so.6\\n'\nexit 1\n")
file(CHMOD "${WORK_DIR}/readelf" "${WORK_DIR}/ldd-ok" "${WORK_DIR}/ldd-fail"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(probe "${PULP_SOURCE_DIR}/test/cmake/check_plugin_module_no_unresolved.cmake")
foreach(case IN ITEMS ok fail)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DLDD=${WORK_DIR}/ldd-${case}" "-DREADELF=${WORK_DIR}/readelf"
        "-DMODULE=${WORK_DIR}/module.so" -P "${probe}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(case STREQUAL "ok" AND NOT rc EQUAL 0)
        message(FATAL_ERROR "successful probe rejected: ${out} ${err}")
    elseif(case STREQUAL "fail" AND (rc EQUAL 0 OR NOT err MATCHES "ldd -r failed"))
        message(FATAL_ERROR "failed ldd was not rejected for its exit status: ${out} ${err}")
    endif()
endforeach()
