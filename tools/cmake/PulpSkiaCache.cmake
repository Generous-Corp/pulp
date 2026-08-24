# Resolve the manifest-selected Skia cache generation without duplicating the
# pin calculation in CMake. The Python fetcher remains the sole authority for
# platform-to-asset identity; this function only projects its exact destination
# and matching fetch arguments into a fresh configure process.
function(pulp_resolve_skia_cache matrix_platform legacy_suffix out_dest out_fetch_args)
    find_program(_pulp_skia_python NAMES python3 python)
    if(NOT _pulp_skia_python)
        set(${out_dest} "" PARENT_SCOPE)
        set(${out_fetch_args} "" PARENT_SCOPE)
        return()
    endif()

    set(_pulp_keying "$ENV{PULP_SKIA_CACHE_KEYING}")
    if(_pulp_keying STREQUAL "")
        set(_pulp_keying "1")
    endif()
    if(_pulp_keying STREQUAL "1")
        set(_pulp_cache_root "$ENV{PULP_SKIA_CACHE_ROOT}")
        if(_pulp_cache_root STREQUAL "")
            set(_pulp_cache_root "$ENV{HOME}/.cache/pulp/skia")
        endif()
        execute_process(
            COMMAND ${_pulp_skia_python} tools/scripts/fetch_skia_for_release.py
                    ${matrix_platform} --cache-root "${_pulp_cache_root}"
                    --print-cache-dest
            WORKING_DIRECTORY "${PULP_ROOT_DIR}"
            RESULT_VARIABLE _pulp_resolve_result
            OUTPUT_VARIABLE _pulp_cache_dest
            ERROR_VARIABLE _pulp_resolve_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _pulp_resolve_result EQUAL 0 OR _pulp_cache_dest STREQUAL "")
            message(FATAL_ERROR
                "Skia: failed to resolve immutable cache generation: "
                "${_pulp_resolve_error}")
        endif()
        set(_pulp_fetch_args --cache-root "${_pulp_cache_root}")
    else()
        set(_pulp_cache_dest "$ENV{PULP_SKIA_CACHE}")
        if(_pulp_cache_dest STREQUAL "")
            set(_pulp_cache_dest "$ENV{HOME}/.cache/pulp/skia-build${legacy_suffix}")
        endif()
        set(_pulp_fetch_args --dest "${_pulp_cache_dest}")
    endif()

    set(_pulp_locking "$ENV{PULP_SKIA_CACHE_LOCKING}")
    if(_pulp_locking STREQUAL "")
        set(_pulp_locking "1")
    endif()
    if(_pulp_keying STREQUAL "1" OR NOT _pulp_locking STREQUAL "0")
        set(_pulp_lock_timeout "$ENV{PULP_SKIA_CACHE_LOCK_TIMEOUT_SECS}")
        if(_pulp_lock_timeout STREQUAL "")
            set(_pulp_lock_timeout "300")
        endif()
        list(APPEND _pulp_fetch_args --cache-lock-timeout "${_pulp_lock_timeout}")
    endif()

    set(${out_dest} "${_pulp_cache_dest}" PARENT_SCOPE)
    set(${out_fetch_args} "${_pulp_fetch_args}" PARENT_SCOPE)
endfunction()
