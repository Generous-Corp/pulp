# A Pulp SDK installed to a fresh prefix serves its Vellum integration packages
# to a consumer configured from scratch.
#
#   -DPULP_SOURCE_DIR   the source tree (for the consumer project)
#   -DBUILD_DIR         a configured and fully built Pulp build tree with
#                       PULP_ENABLE_PULP_VELLUM and PULP_ENABLE_PULP_VELLUM_AUDIO
#                       on; `cmake --install` needs every target built
#   -DFIXTURE_DIR       scratch directory, removed and recreated
#   -DVELLUM_PREFIX     the Vellum SDK the build was configured against
#
# The scenario installs the build to <FIXTURE_DIR>/prefix, configures
# tools/validation/sdk-smoke/vellum-installed against that prefix alone, builds
# it and runs it. A second consumer is configured with Vellum made unfindable
# and must be refused, so a green first run cannot come from a Vellum that was
# never needed.
foreach(_input PULP_SOURCE_DIR BUILD_DIR FIXTURE_DIR VELLUM_PREFIX)
    if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
        message(FATAL_ERROR "${_input} is required")
    endif()
endforeach()
if(NOT EXISTS "${BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "BUILD_DIR '${BUILD_DIR}' is not a configured build tree")
endif()
if(NOT EXISTS "${VELLUM_PREFIX}/lib/cmake/Vellum/VellumConfig.cmake")
    message(FATAL_ERROR "VELLUM_PREFIX '${VELLUM_PREFIX}' holds no Vellum package")
endif()

set(_prefix "${FIXTURE_DIR}/prefix")
file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")

execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${_prefix}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (a target is probably not built):\n${_out}${_err}")
endif()

set(_config "${_prefix}/lib/cmake/Pulp/PulpConfig.cmake")
if(NOT EXISTS "${_config}")
    message(FATAL_ERROR "the install holds no PulpConfig.cmake")
endif()
file(READ "${_config}" _config_text)
if(NOT _config_text MATCHES "set\\(PULP_SDK_HAS_VELLUM_BRIDGE \"ON\"\\)")
    message(FATAL_ERROR "the installed SDK does not report the Vellum bridge; was the build configured with PULP_ENABLE_PULP_VELLUM?")
endif()
foreach(_header pulp/vellum/about_notices.hpp pulp/vellum_audio/graph_scale.hpp)
    if(NOT EXISTS "${_prefix}/include/${_header}")
        message(FATAL_ERROR "the install does not ship include/${_header}")
    endif()
endforeach()
foreach(_archive libpulp-vellum.a libpulp-vellum-audio.a)
    if(NOT EXISTS "${_prefix}/lib/${_archive}")
        message(FATAL_ERROR "the install does not ship lib/${_archive}")
    endif()
endforeach()

set(_isolation
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
set(_clean_env "${CMAKE_COMMAND}" -E env --unset=Vellum_DIR --unset=Pulp_DIR
    --unset=CMAKE_PREFIX_PATH)
set(_generator)
find_program(_ninja ninja)
if(_ninja)
    set(_generator -G Ninja)
endif()
set(_consumer "${PULP_SOURCE_DIR}/tools/validation/sdk-smoke/vellum-installed")

# Control: with Vellum unfindable the same consumer must not configure.
execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" ${_generator}
        -S "${_consumer}" -B "${FIXTURE_DIR}/no-vellum" -DCMAKE_BUILD_TYPE=Release
        -DPulp_DIR=${_prefix}/lib/cmake/Pulp ${_isolation}
        -DCMAKE_DISABLE_FIND_PACKAGE_Vellum=ON
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "control: the consumer configured with Vellum unfindable:\n${_out}${_err}")
endif()
message(STATUS "control: refused without Vellum")

execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" ${_generator}
        -S "${_consumer}" -B "${FIXTURE_DIR}/build" -DCMAKE_BUILD_TYPE=Release
        -DPulp_DIR=${_prefix}/lib/cmake/Pulp -DCMAKE_PREFIX_PATH=${VELLUM_PREFIX} ${_isolation}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "consumer configure failed:\n${_out}${_err}")
endif()
message(STATUS "${_out}")

set(_jobs 4)
if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FIXTURE_DIR}/build" -j ${_jobs}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "consumer build or run failed:\n${_out}${_err}")
endif()
foreach(_line IN ITEMS
        "Pulp::vellum-audio -> LogFrequencyScale midpoint x=500.0"
        "Pulp::vellum -> make_about_model rows=1")
    if(NOT _out MATCHES "${_line}")
        message(FATAL_ERROR "the consumer did not print '${_line}':\n${_out}")
    endif()
endforeach()
message(STATUS "installed SDK: both Vellum integration packages configured, built and ran from ${_prefix}")
message(STATUS "vellum-installed-sdk-smoke: ok")
