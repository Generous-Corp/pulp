# PulpSkiaIcuData.cmake
# Swap Skia's libskunicode_icu.a for a copy with trimmed ICU data.
#
# _pulp_skia_subset_icu_data(<libs_var>)
#   Replaces the libskunicode_icu.a entry in <libs_var> with a copy under
#   ${CMAKE_BINARY_DIR}/pulp-skia-icu-subset/ whose embedded ICU data keeps
#   only what the linked ICU code reads (scripts/icu_data_subset.py holds the
#   policy). The copy is regenerated when the source archive, the script, or
#   the policy changes. When Python, the script, or the rewrite is unavailable
#   the list is left unchanged and a warning names the reason, so a build never
#   fails here; the icu-data-subset-linked and plugin-footprint tests, which
#   are registered whenever PULP_ICU_DATA_SUBSET is ON, then fail on the size.

function(_pulp_skia_subset_icu_data libs_var)
    set(_libs "${${libs_var}}")
    set(_src "")
    foreach(_lib IN LISTS _libs)
        get_filename_component(_name "${_lib}" NAME)
        if(_name STREQUAL "libskunicode_icu.a")
            set(_src "${_lib}")
        endif()
    endforeach()
    if(NOT _src OR NOT EXISTS "${_src}")
        return()
    endif()

    set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/icu_data_subset.py")
    if(NOT EXISTS "${_script}")
        message(WARNING "Skia ICU data subset: ${_script} is missing; linking the full ICU data")
        return()
    endif()
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter QUIET)
    endif()
    if(NOT Python3_EXECUTABLE)
        message(WARNING "Skia ICU data subset: Python 3 not found; linking the full ICU data")
        return()
    endif()

    file(SHA256 "${_script}" _script_hash)
    file(TIMESTAMP "${_src}" _src_stamp "%Y%m%dT%H%M%S" UTC)
    file(SIZE "${_src}" _src_size)
    string(SHA256 _key "${_src}|${_src_size}|${_src_stamp}|${_script_hash}")
    string(SUBSTRING "${_key}" 0 16 _key)
    set(_dir "${CMAKE_BINARY_DIR}/pulp-skia-icu-subset/${_key}")
    set(_out "${_dir}/libskunicode_icu.a")

    if(NOT EXISTS "${_out}")
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/pulp-skia-icu-subset")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${_script}"
                --archive "${_src}"
                --output "${_out}"
                --work-dir "${_dir}/work"
                --report "${_dir}/report.json"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr)
        file(REMOVE_RECURSE "${_dir}/work")
        if(NOT _rc EQUAL 0 OR NOT EXISTS "${_out}")
            file(REMOVE "${_out}")
            string(STRIP "${_stderr}" _stderr)
            message(WARNING
                "Skia ICU data subset failed (${_stderr}); linking the full ICU data")
            return()
        endif()
        string(STRIP "${_stdout}" _stdout)
        string(REPLACE "\n" "; " _stdout "${_stdout}")
        message(STATUS "Skia ICU data subset: ${_stdout}")
    endif()

    list(TRANSFORM _libs REPLACE "^.*/libskunicode_icu\\.a$" "${_out}")
    set(${libs_var} "${_libs}" PARENT_SCOPE)
endfunction()
