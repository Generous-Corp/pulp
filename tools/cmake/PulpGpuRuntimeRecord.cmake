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

# pulp_check_gpu_runtime_archive(<prefix> <archive> <locked-digest>)
#
# Binds an installed gpu-runtime prefix to the archive digest pulp.lock names
# (vellum.gpu_runtime_digest). The record's source_commit is self-reported, so
# it cannot vouch for the bytes. The archive is the thing the lock hashes, so:
#   1. the archive's own sha256 must equal the locked digest;
#   2. the gpu-runtime-artifact.json inside the archive must be byte-identical
#      to the prefix's, so the prefix claims exactly the locked archive's files;
#   3. every file that record lists must exist in the prefix with its recorded
#      sha256, and the prefix holds nothing else
#      (no extra or symlinked file can ride along).
# A prefix whose archive is not supplied, or does not match, stops configure.
function(pulp_check_gpu_runtime_archive prefix archive locked_digest)
    if(archive STREQUAL "")
        message(FATAL_ERROR
            "Pulp: PULP_GPU_RUNTIME_PREFIX (${prefix}) cannot be matched to pulp.lock "
            "vellum.gpu_runtime_digest: set PULP_GPU_RUNTIME_ARCHIVE to the "
            "gpu-runtime archive the prefix was extracted from")
    endif()
    if(NOT EXISTS "${archive}")
        message(FATAL_ERROR "Pulp: gpu-runtime archive does not exist: ${archive}")
    endif()
    file(SHA256 "${archive}" _archive_digest)
    if(NOT _archive_digest STREQUAL locked_digest)
        message(FATAL_ERROR
            "Pulp: gpu-runtime archive digest mismatch against pulp.lock "
            "vellum.gpu_runtime_digest (lock ${locked_digest}, file ${_archive_digest})")
    endif()
    string(SHA256 _scratch_id "${archive}${prefix}")
    set(_scratch "${CMAKE_BINARY_DIR}/pulp-gpu-runtime-archive-${_scratch_id}")
    file(REMOVE_RECURSE "${_scratch}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${_scratch}"
        PATTERNS gpu-runtime-artifact.json)
    if(NOT EXISTS "${_scratch}/gpu-runtime-artifact.json")
        message(FATAL_ERROR "Pulp: ${archive} carries no gpu-runtime-artifact.json")
    endif()
    file(SHA256 "${_scratch}/gpu-runtime-artifact.json" _archive_record)
    file(SHA256 "${prefix}/gpu-runtime-artifact.json" _prefix_record)
    file(REMOVE_RECURSE "${_scratch}")
    if(NOT _archive_record STREQUAL _prefix_record)
        message(FATAL_ERROR
            "Pulp: the gpu-runtime at ${prefix} is not the pulp.lock vellum.gpu_runtime_digest "
            "archive: its gpu-runtime-artifact.json differs from the archive's")
    endif()
    file(READ "${prefix}/gpu-runtime-artifact.json" _json)
    string(JSON _count ERROR_VARIABLE _error LENGTH "${_json}" files)
    if(_error OR _count EQUAL 0)
        message(FATAL_ERROR "Pulp: ${prefix}/gpu-runtime-artifact.json lists no files to verify")
    endif()
    set(_listed "gpu-runtime-artifact.json")
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE ${_last})
        string(JSON _path GET "${_json}" files ${_index} path)
        string(JSON _sha GET "${_json}" files ${_index} sha256)
        list(APPEND _listed "${_path}")
        if(IS_SYMLINK "${prefix}/${_path}" OR NOT EXISTS "${prefix}/${_path}")
            message(FATAL_ERROR
                "Pulp: the gpu-runtime at ${prefix} lacks ${_path} (or it is a symlink); it "
                "does not match pulp.lock vellum.gpu_runtime_digest")
        endif()
        file(SHA256 "${prefix}/${_path}" _actual)
        if(NOT _actual STREQUAL _sha)
            message(FATAL_ERROR
                "Pulp: ${_path} in the gpu-runtime at ${prefix} was changed after extraction "
                "(recorded ${_sha}, found ${_actual}); it does not match pulp.lock "
                "vellum.gpu_runtime_digest")
        endif()
    endforeach()
    file(GLOB_RECURSE _present RELATIVE "${prefix}" LIST_DIRECTORIES false "${prefix}/*")
    foreach(_file IN LISTS _present)
        if(NOT _file IN_LIST _listed)
            message(FATAL_ERROR
                "Pulp: the gpu-runtime at ${prefix} holds ${_file}, which the locked archive "
                "does not; it does not match pulp.lock vellum.gpu_runtime_digest")
        endif()
    endforeach()
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
# Components are checked here rather than requested from find_package(), so the
# reason a missing Dawn is refused names the Dawn build to fix.
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
           NOT TARGET VellumGpuRuntime::Dawn)
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
