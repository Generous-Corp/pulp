cmake_minimum_required(VERSION 3.24)

# Installed-header consumer for the scalar Control operations. Stages the
# signal headers with the SDK's header install rule (install(DIRECTORY ...
# FILES_MATCHING *.hpp *.h) into include/pulp), then builds a consumer that
# sees only that prefix:
#   - canonical names build with deprecation promoted to an error and run;
#   - deprecated indexed spellings still build and return canonical bits;
#   - the deprecated spellings fail a deprecation-as-error build (negative
#     control: the deprecation actually reaches a consumer).
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_root "${PULP_BUILD_DIR}/control-scalar-consumer")
set(_prefix "${_root}/prefix")
set(_fixture "${PULP_SOURCE_DIR}/test/fixtures/control_scalar_consumer")
file(REMOVE_RECURSE "${_root}")

file(INSTALL DESTINATION "${_prefix}/include/pulp" TYPE DIRECTORY
    FILES "${PULP_SOURCE_DIR}/core/signal/include/pulp/"
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
if(NOT EXISTS "${_prefix}/include/pulp/signal/control_math.hpp" OR
   NOT EXISTS "${_prefix}/include/pulp/signal/indexed_control.hpp")
    message(FATAL_ERROR "control scalar consumer: headers were not staged into ${_prefix}")
endif()

set(_config_args "-DPULP_INCLUDE_DIR=${_prefix}/include" -DCMAKE_BUILD_TYPE=Release)
if(PULP_PARENT_GENERATOR)
    list(APPEND _config_args -G "${PULP_PARENT_GENERATOR}")
endif()
if(PULP_PARENT_CXX_COMPILER)
    list(APPEND _config_args "-DCMAKE_CXX_COMPILER=${PULP_PARENT_CXX_COMPILER}")
endif()

function(_control_scalar_run description)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "control scalar consumer: ${description} failed (${_rc})\n${_out}\n${_err}")
    endif()
    set(_control_scalar_output "${_out}${_err}" PARENT_SCOPE)
    message(STATUS "control scalar consumer: ${description} passed")
endfunction()

set(_build "${_root}/build")
_control_scalar_run("configure" "${CMAKE_COMMAND}" -S "${_fixture}" -B "${_build}"
    ${_config_args})
_control_scalar_run("build" "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
    "${CMAKE_COMMAND}" --build "${_build}" --config Release)

foreach(_exe control_scalar_canonical control_scalar_legacy)
    set(_path "${_build}/${_exe}")
    if(NOT EXISTS "${_path}")
        set(_path "${_build}/Release/${_exe}.exe")
    endif()
    _control_scalar_run("run ${_exe}" "${_path}")
    string(REPLACE "control_scalar_" "" _kind "${_exe}")
    if(NOT _control_scalar_output MATCHES "control_scalar_${_kind}_consumer=ok")
        message(FATAL_ERROR "control scalar consumer: ${_exe} did not report ok\n${_control_scalar_output}")
    endif()
endforeach()

set(_strict "${_root}/strict")
_control_scalar_run("configure strict" "${CMAKE_COMMAND}" -S "${_fixture}" -B "${_strict}"
    ${_config_args} -DCONTROL_SCALAR_LEGACY_STRICT=ON)
# The strict build goes through the governed-build wrapper like every other
# build here. The wrapper passes the compiler's exit status and diagnostics
# through, and reports "build failed rc=" only for a build it actually ran, so
# requiring that line proves the expected failure is the governed compile and
# not a pre-lease refusal or an ungoverned build.
execute_process(COMMAND "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
        "${CMAKE_COMMAND}" --build "${_strict}" --config Release
        --target control_scalar_legacy_strict
    RESULT_VARIABLE _strict_rc OUTPUT_VARIABLE _strict_out ERROR_VARIABLE _strict_err)
set(_strict_log "${_strict_out}${_strict_err}")
if(_strict_rc EQUAL 0)
    message(FATAL_ERROR "control scalar consumer: deprecated spellings built with deprecation "
        "as an error; the deprecation does not reach consumers")
endif()
foreach(_needle "[governed-build] build failed rc="
        control_minimum control_maximum control_safe_divide finite_control_sample
        "pulp/signal/control_math.hpp")
    string(FIND "${_strict_log}" "${_needle}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "control scalar consumer: strict build failed without naming "
            "${_needle}\n${_strict_log}")
    endif()
endforeach()
message(STATUS "control scalar consumer: deprecated spellings refused under deprecation-as-error")
message(STATUS "control_scalar_installed_consumer_verified=true")
