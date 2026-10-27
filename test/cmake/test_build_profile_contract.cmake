# Proves the PULP_PROFILE contract (tools/cmake/PulpProfiles.cmake) against a
# fixture project that declares every governed option ON:
#   release seeds every enforced option OFF and leaves the neutral ones alone;
#   an explicit contradicting -D is refused with the profile diagnostic;
#   a stale dev cache is refused the same way;
#   PULP_PROFILE_ALLOW_OVERRIDE turns the refusal into a warning naming the option;
#   a cached PULP_DEV_BUILD=ON (the pulp CLI's developer-loop configure) is refused;
#   an unknown profile is refused;
#   the root CMakeLists.txt wires the module around its governed options.
cmake_minimum_required(VERSION 3.24)

if(NOT PULP_SRC_DIR OR NOT FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SRC_DIR and FIXTURE_DIR are required")
endif()

set(_fixture "${PULP_SRC_DIR}/test/cmake/fixtures/build_profile")
file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")

# configure(<build-subdir> <result-var> <output-var> [extra -D args...])
function(_configure subdir result_var output_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_fixture}" -B "${FIXTURE_DIR}/${subdir}"
            "-DPULP_SRC_DIR=${PULP_SRC_DIR}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(${result_var} "${_result}" PARENT_SCOPE)
    set(${output_var} "${_stdout}\n${_stderr}" PARENT_SCOPE)
endfunction()

function(_cache_value subdir name out_var)
    file(STRINGS "${FIXTURE_DIR}/${subdir}/CMakeCache.txt" _line REGEX "^${name}:[A-Z]+=")
    string(REGEX REPLACE "^${name}:[A-Z]+=" "" _value "${_line}")
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

set(_enforced
    PULP_BUILD_TESTS PULP_BUILD_EXAMPLES PULP_ENABLE_AUDIO_PROBES PULP_DEV_BRIDGE_EXEC
    PULP_DEV_BUILD PULP_TRACING PULP_ENABLE_FUZZING PULP_BENCHMARK PULP_RT_WORK_COUNTERS)
set(_neutral PULP_ENABLE_DESIGN_IMPORT PULP_ENABLE_INSPECTOR)

# Control: the dev profile leaves every fixture option at its declared ON, so a
# later OFF is the release profile's doing and not the fixture's.
_configure(dev _result _output)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "dev profile configure failed:\n${_output}")
endif()
foreach(_option IN LISTS _enforced _neutral)
    _cache_value(dev ${_option} _value)
    if(NOT _value STREQUAL "ON")
        message(FATAL_ERROR "dev profile changed ${_option} to '${_value}'")
    endif()
endforeach()
_cache_value(dev PULP_PROFILE _value)
if(NOT _value STREQUAL "dev" OR NOT _output MATCHES "Pulp profile: dev \\(")
    message(FATAL_ERROR "dev profile was not recorded:\n${_output}")
endif()
# The list above is this test's copy of PULP_PROFILE_ENFORCED_OPTIONS; an option
# added to the module without being added here would be enforced but unproven.
file(READ "${FIXTURE_DIR}/dev/enforced-options.txt" _module_enforced)
if(NOT _module_enforced STREQUAL _enforced)
    message(FATAL_ERROR
        "PULP_PROFILE_ENFORCED_OPTIONS is '${_module_enforced}' but this test "
        "proves '${_enforced}'; update _enforced to match")
endif()

# The compile-time axis every gated reader keys on follows the option: ON under dev.
_cache_value(dev PULP_DEV_BUILD _value)
if(NOT _value)
    message(FATAL_ERROR "dev profile cached PULP_DEV_BUILD='${_value}' (expected ON)")
endif()

# (a) release seeds every enforced option OFF; neutral options keep their default.
_configure(release _result _output -DPULP_PROFILE=release)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "release profile configure failed:\n${_output}")
endif()
foreach(_option IN LISTS _enforced)
    _cache_value(release ${_option} _value)
    if(NOT _value STREQUAL "OFF")
        message(FATAL_ERROR "release profile left ${_option}='${_value}' (expected OFF)")
    endif()
endforeach()
foreach(_option IN LISTS _neutral)
    _cache_value(release ${_option} _value)
    if(NOT _value STREQUAL "ON")
        message(FATAL_ERROR "release profile touched neutral ${_option} ('${_value}')")
    endif()
endforeach()
_cache_value(release PULP_PROFILE _value)
if(NOT _value STREQUAL "release")
    message(FATAL_ERROR "PULP_PROFILE was not cached as release ('${_value}')")
endif()
# ... and 0 under release, so the environment-variable readers compile out.
_cache_value(release PULP_DEV_BUILD _value)
if(_value)
    message(FATAL_ERROR "release profile cached PULP_DEV_BUILD='${_value}' (expected OFF)")
endif()
if(NOT _output MATCHES "Pulp profile: release \\(PULP_ENABLE_DESIGN_IMPORT=ON PULP_ENABLE_INSPECTOR=ON\\)")
    message(FATAL_ERROR "release profile did not record the neutral options:\n${_output}")
endif()
_cache_value(release PULP_PROFILE_OVERRIDDEN _value)
if(NOT _value STREQUAL "OFF")
    message(FATAL_ERROR "clean release configure published PULP_PROFILE_OVERRIDDEN='${_value}'")
endif()
_cache_value(release PULP_PROFILE_NEUTRAL_RECORD _value)
if(NOT _value STREQUAL "PULP_ENABLE_DESIGN_IMPORT=ON PULP_ENABLE_INSPECTOR=ON")
    message(FATAL_ERROR "PULP_PROFILE_NEUTRAL_RECORD is '${_value}'")
endif()

# (b) an explicit contradicting -D is refused, naming the option.
_configure(contradict _result _output -DPULP_PROFILE=release -DPULP_DEV_BRIDGE_EXEC=ON)
if(_result EQUAL 0)
    message(FATAL_ERROR "release profile accepted -DPULP_DEV_BRIDGE_EXEC=ON")
endif()
if(NOT _output MATCHES "PULP_PROFILE=release requires these options OFF" OR
   NOT _output MATCHES "PULP_DEV_BRIDGE_EXEC=ON")
    message(FATAL_ERROR "contradicting -D failed without the profile diagnostic:\n${_output}")
endif()
# The other enforced options agree, so the diagnostic lists only the offender.
if(_output MATCHES "PULP_BUILD_TESTS=ON")
    message(FATAL_ERROR "profile diagnostic named an option that agreed:\n${_output}")
endif()

# A stale cache is refused the same way: a build directory configured as dev
# carries ON for every option, and reconfiguring it as release must not
# silently inherit those values.
_configure(dev _result _output -DPULP_PROFILE=release)
if(_result EQUAL 0)
    message(FATAL_ERROR "release profile accepted a stale dev cache")
endif()
if(NOT _output MATCHES "PULP_PROFILE=release requires these options OFF" OR
   NOT _output MATCHES "PULP_BUILD_TESTS=ON" OR NOT _output MATCHES "PULP_TRACING=ON")
    message(FATAL_ERROR "stale cache failed without the profile diagnostic:\n${_output}")
endif()
# Passing the stale options OFF explicitly satisfies the profile again.
set(_offs "")
foreach(_option IN LISTS _enforced)
    list(APPEND _offs "-D${_option}=OFF")
endforeach()
_configure(dev _result _output -DPULP_PROFILE=release ${_offs})
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "release profile refused a cache whose options were passed OFF:\n${_output}")
endif()

# (c) the override flag allows the contradiction with a warning naming it.
_configure(override _result _output
    -DPULP_PROFILE=release -DPULP_PROFILE_ALLOW_OVERRIDE=ON -DPULP_DEV_BRIDGE_EXEC=ON)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "PULP_PROFILE_ALLOW_OVERRIDE did not allow the configure:\n${_output}")
endif()
if(NOT _output MATCHES "overridden by PULP_PROFILE_ALLOW_OVERRIDE" OR
   NOT _output MATCHES "PULP_DEV_BRIDGE_EXEC=ON")
    message(FATAL_ERROR "override configure did not warn about the override:\n${_output}")
endif()
_cache_value(override PULP_DEV_BRIDGE_EXEC _value)
if(NOT _value STREQUAL "ON")
    message(FATAL_ERROR "override did not keep PULP_DEV_BRIDGE_EXEC=ON ('${_value}')")
endif()
_cache_value(override PULP_BUILD_TESTS _value)
if(NOT _value STREQUAL "OFF")
    message(FATAL_ERROR "override changed an option that was not overridden ('${_value}')")
endif()
# The override is published so provenance writers treat the build as dev
# although PULP_PROFILE still says release.
_cache_value(override PULP_PROFILE_OVERRIDDEN _value)
if(NOT _value STREQUAL "ON")
    message(FATAL_ERROR "override did not publish PULP_PROFILE_OVERRIDDEN=ON ('${_value}')")
endif()
_cache_value(override PULP_PROFILE _value)
if(NOT _value STREQUAL "release")
    message(FATAL_ERROR "override changed PULP_PROFILE ('${_value}')")
endif()

# The source-install stamp records an overridden release as build_profile dev.
# Render the template both ways and run each stamp into its own prefix.
set(_stamp_in "${PULP_SRC_DIR}/tools/cmake/PulpStampSourceInstall.cmake.in")
set(PULP_SOURCE_INSTALL_SHA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
set(PULP_SOURCE_INSTALL_DIRTY false)
set(PULP_SOURCE_INSTALL_BRIDGE_EXEC false)
# The stamp also records the target platform; this test exercises the profile only.
set(PULP_SOURCE_INSTALL_OS "darwin")
set(PULP_SOURCE_INSTALL_ARCH "arm64")
set(PULP_PROFILE "release")
foreach(_case "OFF;release" "ON;dev")
    list(GET _case 0 PULP_PROFILE_OVERRIDDEN)
    list(GET _case 1 _expected_profile)
    set(_stamp "${FIXTURE_DIR}/stamp-${PULP_PROFILE_OVERRIDDEN}.cmake")
    configure_file("${_stamp_in}" "${_stamp}" @ONLY)
    set(ENV{DESTDIR} "${FIXTURE_DIR}/stamp-${PULP_PROFILE_OVERRIDDEN}-prefix")
    file(MAKE_DIRECTORY "$ENV{DESTDIR}/sdk")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DCMAKE_INSTALL_PREFIX=/sdk -P "${_stamp}"
        RESULT_VARIABLE _stamp_result OUTPUT_VARIABLE _stamp_out ERROR_VARIABLE _stamp_err)
    unset(ENV{DESTDIR})
    if(NOT _stamp_result EQUAL 0)
        message(FATAL_ERROR "stamp (overridden=${PULP_PROFILE_OVERRIDDEN}) failed:\n${_stamp_out}\n${_stamp_err}")
    endif()
    file(READ "${FIXTURE_DIR}/stamp-${PULP_PROFILE_OVERRIDDEN}-prefix/sdk/sdk-provenance.json" _marker)
    string(JSON _recorded GET "${_marker}" build_profile)
    if(NOT _recorded STREQUAL _expected_profile)
        message(FATAL_ERROR
            "stamp with PULP_PROFILE_OVERRIDDEN=${PULP_PROFILE_OVERRIDDEN} recorded "
            "build_profile '${_recorded}', expected '${_expected_profile}'")
    endif()
endforeach()

# A cached PULP_DEV_BUILD=ON is refused on its own. The pulp CLI's developer
# loop writes exactly this cache (-DPULP_DEV_BUILD=ON into build/), so a
# release configure into that directory must not inherit the on-disk UI_SCRIPT
# hot-reload path. Everything else is cached OFF so only this option can fail.
set(_dev_build_offs "")
foreach(_option IN LISTS _enforced)
    if(NOT _option STREQUAL "PULP_DEV_BUILD")
        list(APPEND _dev_build_offs "-D${_option}=OFF")
    endif()
endforeach()
_configure(dev-build _result _output -DPULP_DEV_BUILD=ON ${_dev_build_offs})
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "dev profile refused PULP_DEV_BUILD=ON:\n${_output}")
endif()
_configure(dev-build _result _output -DPULP_PROFILE=release)
if(_result EQUAL 0)
    message(FATAL_ERROR "release profile accepted a cached PULP_DEV_BUILD=ON")
endif()
if(NOT _output MATCHES "PULP_PROFILE=release requires these options OFF" OR
   NOT _output MATCHES "PULP_DEV_BUILD=ON")
    message(FATAL_ERROR "cached PULP_DEV_BUILD=ON failed without the profile diagnostic:\n${_output}")
endif()
if(_output MATCHES "PULP_BUILD_TESTS=ON" OR _output MATCHES "PULP_DEV_BRIDGE_EXEC=ON")
    message(FATAL_ERROR "profile diagnostic named an option that agreed:\n${_output}")
endif()

# An unknown profile is refused before any option is seeded.
_configure(unknown _result _output -DPULP_PROFILE=ship)
if(_result EQUAL 0 OR NOT _output MATCHES "is not a Pulp build profile")
    message(FATAL_ERROR "unknown profile was not refused:\n${_output}")
endif()

# The root CMakeLists.txt includes the module before the first governed option
# and validates after the last one, so the fixture above describes the real
# configure and not only itself.
file(READ "${PULP_SRC_DIR}/CMakeLists.txt" _root)
string(FIND "${_root}" "tools/cmake/PulpProfiles.cmake)" _include_at)
string(FIND "${_root}" "option(PULP_BUILD_TESTS" _first_option_at)
string(FIND "${_root}" "option(PULP_DEV_BUILD" _last_option_at)
string(FIND "${_root}" "\npulp_profile_validate()\n" _validate_at)
if(_include_at EQUAL -1 OR _first_option_at EQUAL -1 OR
   _last_option_at EQUAL -1 OR _validate_at EQUAL -1 OR
   NOT _include_at LESS _first_option_at OR NOT _last_option_at LESS _validate_at)
    message(FATAL_ERROR
        "CMakeLists.txt must include PulpProfiles.cmake before option(PULP_BUILD_TESTS) "
        "and call pulp_profile_validate() after option(PULP_DEV_BUILD)")
endif()
