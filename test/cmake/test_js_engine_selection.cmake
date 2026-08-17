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

function(_expect_engine_failure name prelude expected_error)
    set(_probe "${_probe_dir}/${name}.cmake")
    # The probe must stand in the same policy context a real configure gives the
    # module, or it does not exercise the real validation. The module selects
    # with `if(... IN_LIST ...)`, which only behaves as an operator once CMP0057
    # is NEW — a project gets that from the top-level cmake_minimum_required,
    # but a `cmake -P` script inherits nothing and falls back to whatever the
    # running CMake defaults to. Without this line the IN_LIST test silently
    # evaluates the OLD way, the module never reaches its FATAL_ERROR, and the
    # probe fails for an unrelated reason while the assertion below reports it
    # as a wrong *message*.
    file(WRITE "${_probe}"
        "cmake_policy(SET CMP0057 NEW)\n"
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
    # Distinguish "the probe said nothing" from "the probe said the wrong
    # thing". A probe that dies before reaching the module — unwritable dir,
    # missing module, policy fallback — produces no diagnostic of its own, and
    # reporting that as a wrong message sends the next reader looking for a
    # text mismatch that does not exist.
    string(STRIP "${_output}" _stripped_output)
    if(_stripped_output STREQUAL "")
        message(FATAL_ERROR
            "${name} failed with status ${_status} but produced no diagnostic at all. "
            "The probe did not reach the module's validation; this is not a message mismatch.")
    endif()
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
