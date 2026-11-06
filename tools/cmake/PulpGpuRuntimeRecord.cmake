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
