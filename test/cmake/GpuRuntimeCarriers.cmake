# pulp_gpu_runtime_write_carriers(<root-dir> <out-file>)
#
# Writes the facts gpu-runtime-mode-consistency checks, one line each, for every
# buildsystem target under <root-dir>:
#
#   <target> <TRUE|FALSE>               carries PULP_GPU_RUNTIME directly, and
#                                       whether it links VellumGpuRuntime::DeviceApi
#   benchmark-only <target> <type> <b>  defines PULP_BENCHMARK itself while the
#                                       global PULP_BENCHMARK is off; <b> is
#                                       whether it also names GPU_RUNTIME_BENCHMARK
#   benchmark-interface <target>        passes PULP_BENCHMARK on to its consumers
#   benchmark-unreadable <target>       names PULP_BENCHMARK in a generator expression
#   benchmark-consumer <target> <lib>   links, or takes the objects of, a
#                                       benchmark-only library
#
# Consumers test both benchmark macros with #ifdef, so any spelling (`=0`
# included) is the benchmark flavour; a compile option `-D...` counts the same
# as a compile definition.
function(_pulp_gpu_runtime_collect_targets dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _pulp_gpu_runtime_collect_targets("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Sets <out-named> TRUE when an entry of <defs> or <options> defines <macro>,
# and <out-unreadable> TRUE when a generator expression mentions it.
function(_pulp_gpu_runtime_names_macro macro defs options out_named out_unreadable)
    set(_named FALSE)
    set(_unreadable FALSE)
    foreach(_entry IN LISTS defs options)
        if(_entry MATCHES "\\$<" AND _entry MATCHES "${macro}")
            set(_unreadable TRUE)
        endif()
    endforeach()
    foreach(_def IN LISTS defs)
        if(_def MATCHES "^${macro}(=.*)?$")
            set(_named TRUE)
        endif()
    endforeach()
    foreach(_option IN LISTS options)
        if(_option MATCHES "^[-/]D[ ]*${macro}(=.*)?$")
            set(_named TRUE)
        endif()
    endforeach()
    set(${out_named} ${_named} PARENT_SCOPE)
    set(${out_unreadable} ${_unreadable} PARENT_SCOPE)
endfunction()

function(_pulp_gpu_runtime_property target property out_var)
    get_target_property(_value ${target} ${property})
    if(NOT _value)
        set(_value "")
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(pulp_gpu_runtime_write_carriers root out_file)
    _pulp_gpu_runtime_collect_targets("${root}" _targets)
    set(_lines "")
    set(_benchmark_only "")
    foreach(_target IN LISTS _targets)
        if(NOT PULP_BENCHMARK)
            _pulp_gpu_runtime_property(${_target} COMPILE_DEFINITIONS _defs)
            _pulp_gpu_runtime_property(${_target} COMPILE_OPTIONS _options)
            _pulp_gpu_runtime_names_macro(PULP_BENCHMARK "${_defs}" "${_options}" _local _local_unreadable)
            _pulp_gpu_runtime_property(${_target} INTERFACE_COMPILE_DEFINITIONS _idefs)
            _pulp_gpu_runtime_property(${_target} INTERFACE_COMPILE_OPTIONS _ioptions)
            _pulp_gpu_runtime_names_macro(PULP_BENCHMARK "${_idefs}" "${_ioptions}" _interface _interface_unreadable)
            if(_local_unreadable OR _interface_unreadable)
                string(APPEND _lines "benchmark-unreadable ${_target}\n")
            endif()
            if(_interface)
                string(APPEND _lines "benchmark-interface ${_target}\n")
            endif()
            if(_local)
                get_target_property(_type ${_target} TYPE)
                _pulp_gpu_runtime_names_macro(GPU_RUNTIME_BENCHMARK "${_defs}" "${_options}" _grb _grb_unreadable)
                string(APPEND _lines "benchmark-only ${_target} ${_type} ${_grb}\n")
                list(APPEND _benchmark_only ${_target})
            endif()
        endif()
        set(_carries FALSE)
        set(_links FALSE)
        foreach(_prop COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(_carrier_defs ${_target} ${_prop})
            if(_carrier_defs AND "${_carrier_defs}" MATCHES "PULP_GPU_RUNTIME")
                set(_carries TRUE)
            endif()
        endforeach()
        if(NOT _carries)
            continue()
        endif()
        foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_libs ${_target} ${_prop})
            if(_libs AND "VellumGpuRuntime::DeviceApi" IN_LIST _libs)
                set(_links TRUE)
            endif()
        endforeach()
        string(APPEND _lines "${_target} ${_links}\n")
    endforeach()
    # A benchmark-only library is compile-only: nothing may link it or take its
    # objects, or one image would hold both GpuCompute vtables.
    foreach(_lib IN LISTS _benchmark_only)
        foreach(_target IN LISTS _targets)
            if(_target STREQUAL _lib)
                continue()
            endif()
            foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES SOURCES INTERFACE_SOURCES)
                _pulp_gpu_runtime_property(${_target} ${_prop} _items)
                if(_lib IN_LIST _items OR "${_items}" MATCHES "\\$<TARGET_OBJECTS:${_lib}>")
                    string(APPEND _lines "benchmark-consumer ${_target} ${_lib}\n")
                    break()
                endif()
            endforeach()
        endforeach()
    endforeach()
    file(WRITE "${out_file}" "${_lines}")
endfunction()
