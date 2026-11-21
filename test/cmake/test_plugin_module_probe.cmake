if(NOT PULP_SOURCE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and WORK_DIR are required")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/module.so" "fixture")
file(WRITE "${WORK_DIR}/readelf" "#!/bin/sh\nprintf ' (NULL)\\n'\n")
file(WRITE "${WORK_DIR}/ldd-ok" "#!/bin/sh\nprintf ' libc.so.6 => /lib/libc.so.6\\n'\nexit 0\n")
file(WRITE "${WORK_DIR}/ldd-fail" "#!/bin/sh\nprintf ' libc.so.6 => /lib/libc.so.6\\n'\nexit 1\n")
file(WRITE "${WORK_DIR}/ldd-undef-exit0" "#!/bin/sh\nprintf ' libc.so.6 => /lib/libc.so.6\\n'\nprintf 'undefined symbol: FcInit\\n'\nexit 0\n")
file(WRITE "${WORK_DIR}/ldd-undef-exit1" "#!/bin/sh\nprintf ' libc.so.6 => /lib/libc.so.6\\n'\nprintf 'undefined symbol: FcInit\\n'\nexit 1\n")
file(WRITE "${WORK_DIR}/ldd-loader-exit0" "#!/bin/sh\nprintf '\\tstatically linked\\n'\nprintf 'undefined symbol: FcInit\\t(/x/module.so)\\n'\nexit 0\n")
file(CHMOD "${WORK_DIR}/ldd-loader-exit0" "${WORK_DIR}/readelf" "${WORK_DIR}/ldd-ok" "${WORK_DIR}/ldd-fail"
    "${WORK_DIR}/ldd-undef-exit0" "${WORK_DIR}/ldd-undef-exit1"
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

# EXPECT=unresolved: bare undefined-symbol text (not in the loader's
# "NAME (object)" form) must be rejected whatever the exit; loader-form lines
# are accepted even with exit 0, which is what glibc's ldd returns for a
# shared object it reports as "statically linked".
foreach(case IN ITEMS undef-exit0 undef-exit1 loader-exit0)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DLDD=${WORK_DIR}/ldd-${case}" "-DREADELF=${WORK_DIR}/readelf"
        "-DMODULE=${WORK_DIR}/module.so" -DEXPECT=unresolved -P "${probe}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(case MATCHES "^undef-" AND (rc EQUAL 0 OR NOT err MATCHES "not in the loader"))
        message(FATAL_ERROR "unresolved mode accepted text that ldd did not print: ${out} ${err}")
    elseif(case STREQUAL "loader-exit0" AND NOT rc EQUAL 0)
        message(FATAL_ERROR "unresolved mode rejected genuine loader output: ${out} ${err}")
    endif()
endforeach()

# A missing ldd yields a non-numeric result string; it must not pass either.
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DLDD=${WORK_DIR}/ldd-missing" "-DREADELF=${WORK_DIR}/readelf"
    "-DMODULE=${WORK_DIR}/module.so" -DEXPECT=unresolved -P "${probe}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
    message(FATAL_ERROR "unresolved mode accepted an ldd that could not run: ${out} ${err}")
endif()
