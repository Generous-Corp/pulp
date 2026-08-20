#[[
Fast negative-contract tests for PULP_JS_ENGINE selection.

The full Pulp configure proves the selected positive backend. These script-mode
probes stop at the real module's early validation and pin the two failure paths
that otherwise require a second host OS or an intentionally invalid build tree.
]]

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(TO_CMAKE_PATH "${_repo_root}/core/view/cmake/PulpViewJsEngines.cmake"
    _engine_module)

set(_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/js-engine-selection-probes")
file(MAKE_DIRECTORY "${_probe_dir}")

# A `cmake -P` script starts with no policy floor, so policies newer than the
# running CMake's baseline default to OLD. The engine module validates with
# `if(... IN_LIST ...)`, which needs CMP0057 NEW; under OLD it fails to parse and
# the probe dies with "Unknown arguments specified" instead of the diagnostic
# under test. A real build always includes the module under the root's
# cmake_minimum_required, so give the probe that same floor rather than testing
# the module in a policy state production never puts it in. Read the floor from
# the root so the two cannot drift apart.
file(READ "${_repo_root}/CMakeLists.txt" _root_cmakelists)
if(NOT _root_cmakelists MATCHES "cmake_minimum_required\\(VERSION ([0-9]+\\.[0-9]+)")
    message(FATAL_ERROR
        "could not read the policy floor from ${_repo_root}/CMakeLists.txt")
endif()
set(_policy_floor "${CMAKE_MATCH_1}")
unset(_root_cmakelists)

function(_expect_engine_failure name prelude expected_error)
    set(_probe "${_probe_dir}/${name}.cmake")
    # The module is written for the policy context its includer establishes, and
    # the real configure supplies that from the top-level cmake_minimum_required.
    # A bare `cmake -P` script has no such context, so CMP0057 is unset and
    # `if(... IN_LIST ...)` is not the IN_LIST operator: the validation the probe
    # exists to exercise never runs, and the probe then fails on the missing
    # diagnostic rather than on the behavior. Give it the project's own baseline
    # so the probe measures the module instead of the ambient policy defaults.
    file(WRITE "${_probe}"
        "cmake_minimum_required(VERSION ${_policy_floor})\n"
        "${prelude}\n"
        "include(\"${_engine_module}\")\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${_probe}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    file(REMOVE "${_probe}")

    if(_status EQUAL 0)
        message(FATAL_ERROR "${name} unexpectedly accepted an invalid engine selection")
    endif()
    set(_output "${_stdout}\n${_stderr}")
    if(NOT _output MATCHES "${expected_error}")
        message(FATAL_ERROR
            "${name} produced the wrong diagnostic. Expected '${expected_error}':\n${_output}")
    endif()
endfunction()

_expect_engine_failure(
    invalid-engine
    "set(PULP_JS_ENGINE definitely-invalid CACHE STRING \"\" FORCE)"
    "PULP_JS_ENGINE must be one of auto, quickjs, jsc, or v8")

_expect_engine_failure(
    non-apple-jsc
    "set(APPLE FALSE)\nset(PULP_ENABLE_JS OFF)\nset(PULP_JS_ENGINE jsc CACHE STRING \"\" FORCE)"
    "PULP_JS_ENGINE=jsc is only supported on Apple platforms")

file(REMOVE_RECURSE "${_probe_dir}")
