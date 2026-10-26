# PulpV8ConsumerDefines.cmake: the preprocessor contract a sealed v8-builder
# provider declares for the code that includes its headers.
#
# Every v8-builder archive carries a manifest.json whose `consumer_defines`
# list records the macros the library was built with and that its public
# headers read inline: V8_ARRAY_BUFFER_INTERNAL_FIELD_COUNT and
# V8_PROMISE_INTERNAL_FIELD_COUNT size embedder fields, CPPGC_CAGED_HEAP and
# CPPGC_POINTER_COMPRESSION select the cppgc member layout, V8_TARGET_ARCH_*
# and V8_COMPRESS_POINTERS (Windows) fix the tagged-field arithmetic. A consumer
# compiled without them still links, and then disagrees with the library about
# object layout at run time. The list is therefore applied verbatim to the
# imported target, and a manifest that cannot supply it fails configure instead
# of silently compiling against the header defaults.

include_guard(GLOBAL)

# Read `consumer_defines` from a sealed provider manifest into out_var.
# FATAL_ERROR when the file is missing, is not JSON, is not marked sealed,
# or the list is absent, empty, or holds anything but NAME / NAME=VALUE tokens.
function(pulp_v8_read_consumer_defines manifest_path out_var)
    set(_where "sealed V8 provider manifest '${manifest_path}'")
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR
            "FindV8: ${_where} does not exist. A sealed v8-builder provider must "
            "ship manifest.json; its consumer_defines are part of the ABI. "
            "Re-fetch with tools/scripts/fetch_v8_for_release.py.")
    endif()
    file(READ "${manifest_path}" _json)

    string(JSON _sealed_type ERROR_VARIABLE _err TYPE "${_json}" sealed)
    if(NOT _err STREQUAL "NOTFOUND")
        message(FATAL_ERROR "FindV8: ${_where} is malformed (no 'sealed' field): ${_err}")
    endif()
    string(JSON _sealed GET "${_json}" sealed)
    if(NOT _sealed_type STREQUAL "BOOLEAN" OR NOT _sealed)
        message(FATAL_ERROR
            "FindV8: ${_where} does not declare sealed=true (got ${_sealed_type} "
            "'${_sealed}'); refusing to guess its consumer ABI.")
    endif()

    string(JSON _type ERROR_VARIABLE _err TYPE "${_json}" consumer_defines)
    if(NOT _err STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "FindV8: ${_where} is malformed (no 'consumer_defines'): ${_err}")
    endif()
    if(NOT _type STREQUAL "ARRAY")
        message(FATAL_ERROR
            "FindV8: ${_where} 'consumer_defines' must be an array, got ${_type}.")
    endif()
    string(JSON _count LENGTH "${_json}" consumer_defines)
    if(_count EQUAL 0)
        message(FATAL_ERROR
            "FindV8: ${_where} declares no consumer_defines; every sealed build "
            "declares at least its target architecture.")
    endif()

    set(_defines "")
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE 0 ${_last})
        string(JSON _item_type TYPE "${_json}" consumer_defines ${_i})
        string(JSON _item GET "${_json}" consumer_defines ${_i})
        # A value is restricted to a plain token so it cannot smuggle a list
        # separator, a quote, or a second flag into the compile line.
        if(NOT _item_type STREQUAL "STRING"
           OR NOT _item MATCHES "^[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_.+-]+)?$")
            message(FATAL_ERROR
                "FindV8: ${_where} consumer_defines[${_i}] is not a NAME or "
                "NAME=VALUE token (${_item_type} '${_item}').")
        endif()
        list(APPEND _defines "${_item}")
    endforeach()
    set(${out_var} "${_defines}" PARENT_SCOPE)
endfunction()

# Attach a sealed provider's consumer_defines to an imported V8 target, so every
# target that links it compiles V8 headers with the library's own configuration.
function(pulp_v8_apply_consumer_defines target manifest_path)
    pulp_v8_read_consumer_defines("${manifest_path}" _defines)
    set_property(TARGET ${target} APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS ${_defines})
    list(JOIN _defines " " _joined)
    message(STATUS "Pulp V8 consumer defines: ${_joined}")
endfunction()
