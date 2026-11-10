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
# VellumGpuRuntime::DeviceApi records the flavour in its interface definitions.
# Every consumer of the macro tests whether it is defined (#ifdef), so any
# GPU_RUNTIME_BENCHMARK entry, whatever its value (=0 included), is the
# benchmark flavour. An entry inside a generator expression cannot be read at
# configure time and is refused rather than guessed.
function(pulp_check_gpu_runtime_benchmark_flavour defs pulp_benchmark)
    set(_runtime OFF)
    foreach(_def IN LISTS defs)
        if(_def MATCHES "\\$<" AND _def MATCHES "GPU_RUNTIME_BENCHMARK")
            message(FATAL_ERROR
                "Pulp: gpu-runtime records GPU_RUNTIME_BENCHMARK through a generator "
                "expression ('${_def}'), so its benchmark flavour cannot be checked")
        elseif(_def MATCHES "^GPU_RUNTIME_BENCHMARK(=.*)?$")
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

# pulp_find_gpu_runtime_package(<version> <dir> <out-error>)
#
# Finds an installed VellumGpuRuntime package of exactly <version> in <dir> and
# checks that it carries the device layer Pulp forwards to. Sets <out-error>
# to an empty string on success, or to the reason it cannot be used.
#
# Components are checked here rather than requested from find_package(): the
# package marks a missing Dawn with VellumGpuRuntime_Device_FOUND FALSE but
# never marks an installed component found, so check_required_components()
# reports the whole package as not found as soon as any component is named.
# GPU_RUNTIME_SKIA_DIR (or GPU_RUNTIME_RENDERER_ARCHIVE) must name the locked
# Dawn build before the call.
function(pulp_find_gpu_runtime_package version dir out_error)
    find_package(VellumGpuRuntime ${version} EXACT CONFIG QUIET
        PATHS "${dir}" NO_DEFAULT_PATH)
    set(_error "")
    if(NOT VellumGpuRuntime_FOUND)
        set(_error "no VellumGpuRuntime ${version} package in ${dir}")
        if(VellumGpuRuntime_NOT_FOUND_MESSAGE)
            string(APPEND _error ": ${VellumGpuRuntime_NOT_FOUND_MESSAGE}")
        endif()
    elseif(NOT VellumGpuRuntime_HAS_DEVICE OR
           (DEFINED VellumGpuRuntime_Device_FOUND AND NOT VellumGpuRuntime_Device_FOUND) OR
           NOT TARGET gpu-runtime-dawn)
        set(_error
            "the VellumGpuRuntime package in ${dir} has no usable device library: it was "
            "built without one, or GPU_RUNTIME_SKIA_DIR ('${GPU_RUNTIME_SKIA_DIR}') does not "
            "hold the locked Dawn build")
    else()
        foreach(_target DeviceApi Bootstrap Device SkiaLogBridge)
            if(NOT TARGET VellumGpuRuntime::${_target})
                set(_error "the VellumGpuRuntime package in ${dir} lacks VellumGpuRuntime::${_target}")
                break()
            endif()
        endforeach()
    endif()
    set(${out_error} "${_error}" PARENT_SCOPE)
endfunction()
