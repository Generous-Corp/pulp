cmake_minimum_required(VERSION 3.24)
if(NOT PULP_BUILD_DIR OR NOT PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()
# The generated stamp binds the configure's exact source/profile identity.
# Keep successive source installs separate; an immutable SDK marker must never
# be overwritten when this gate is rerun after a new source commit.
file(SHA256 "${PULP_BUILD_DIR}/PulpStampSourceInstall.cmake" _identity)
string(SUBSTRING "${_identity}" 0 16 _identity)
set(_root "${PULP_BUILD_DIR}/g11-live-retune-consumer-${_identity}")
function(_run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "G11 installed consumer command failed: ${ARGV} (${_rc})")
    endif()
endfunction()
_run("${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}" --prefix "${_root}/prefix" --config Release)
_run("${CMAKE_COMMAND}" -S "${PULP_SOURCE_DIR}/examples/g11-live-retune-consumer"
    -B "${_root}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_PREFIX_PATH=${_root}/prefix" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
_run("${PULP_SOURCE_DIR}/tools/ci/governed-build.sh" "${CMAKE_COMMAND}" --build "${_root}/build")
_run("${CMAKE_CTEST_COMMAND}" --test-dir "${_root}/build" --output-on-failure --no-tests=error)
