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
    # Read by the icu-data-subset-deterministic test.
    set(PULP_SKIA_ICU_SOURCE_ARCHIVE "${_src}" CACHE INTERNAL
        "Skia's unmodified libskunicode_icu.a")

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

    # The rewrite above runs at configure time. Declare a build rule for the
    # same file too, so a build that finds it deleted (a cleaned directory,
    # tools/scripts/confirm_failure.sh removing a target's build-dir inputs)
    # regenerates it rather than failing with "no known rule to make it".
    if(NOT TARGET pulp-skia-icu-subset)
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${Python3_EXECUTABLE}" "${_script}"
                --archive "${_src}"
                --output "${_out}"
                --work-dir "${_dir}/work"
                --report "${_dir}/report.json"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_dir}/work"
            DEPENDS "${_src}" "${_script}"
            COMMENT "Rewriting Skia's libskunicode_icu.a with trimmed ICU data"
            VERBATIM)
        add_custom_target(pulp-skia-icu-subset DEPENDS "${_out}")
    endif()

    list(TRANSFORM _libs REPLACE "^.*/libskunicode_icu\\.a$" "${_out}")
    set(${libs_var} "${_libs}" PARENT_SCOPE)
endfunction()
