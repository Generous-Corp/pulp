# pulp_check_gpu_runtime_record(<prefix> <commit>)
#
# A published vellum-gpu-runtime archive carries gpu-runtime-artifact.json,
# naming the commit it was built from. Prefix mode builds against exactly the
# pin source mode would fetch, so an installed package without that record, or
# from any other commit, stops configure.
function(pulp_check_gpu_runtime_record prefix commit)
    set(_record "${prefix}/gpu-runtime-artifact.json")
    if(NOT EXISTS "${_record}")
        message(FATAL_ERROR
            "Pulp: ${prefix} has no gpu-runtime-artifact.json, so the commit it was built "
            "from cannot be checked against ${commit}")
    endif()
    file(READ "${_record}" _json)
    string(JSON _built ERROR_VARIABLE _error GET "${_json}" source_commit)
    if(_error OR NOT _built STREQUAL commit)
        message(FATAL_ERROR
            "Pulp: the gpu-runtime at ${prefix} was built from '${_built}', not the pinned ${commit}")
    endif()
endfunction()

# pulp_check_gpu_runtime_benchmark_flavour(<interface-defs> <pulp_benchmark>)
#
# gpu-runtime's benchmark hooks change its GpuCompute vtable, and an image links
# one device library, so its flavour must equal the global PULP_BENCHMARK.
# VellumGpuRuntime::DeviceApi records the flavour in its interface definitions
# as GPU_RUNTIME_BENCHMARK or GPU_RUNTIME_BENCHMARK=1; any other value
# (including =0) is the production flavour.
function(pulp_check_gpu_runtime_benchmark_flavour defs pulp_benchmark)
    set(_runtime OFF)
    foreach(_def IN LISTS defs)
        if(_def MATCHES "^GPU_RUNTIME_BENCHMARK(=1)?$")
            set(_runtime ON)
        endif()
    endforeach()
    if(pulp_benchmark)
        set(_pulp ON)
    else()
        set(_pulp OFF)
    endif()
    if(NOT _runtime STREQUAL _pulp)
        message(FATAL_ERROR
            "Pulp: PULP_BENCHMARK is ${_pulp}, but gpu-runtime was built with "
            "GPU_RUNTIME_BENCHMARK ${_runtime}; the benchmark flavour changes the "
            "GpuCompute vtable, so the two must agree")
    endif()
endfunction()
