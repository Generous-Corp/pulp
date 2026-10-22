# Fixture for test_build_profile_contract.cmake: drives PulpProfiles.cmake
# through real cache semantics (command-line -D, stale cache, option()
# no-ops) without configuring any of Pulp's targets. Every governed option is
# declared ON so the release profile has to override each one.
cmake_minimum_required(VERSION 3.24)
project(PulpBuildProfileFixture LANGUAGES NONE)

if(NOT PULP_SRC_DIR)
    message(FATAL_ERROR "PULP_SRC_DIR is required")
endif()
include("${PULP_SRC_DIR}/tools/cmake/PulpProfiles.cmake")

foreach(_option IN LISTS PULP_PROFILE_ENFORCED_OPTIONS PULP_PROFILE_NEUTRAL_OPTIONS)
    option(${_option} "fixture option" ON)
endforeach()

pulp_profile_validate()

foreach(_option IN LISTS PULP_PROFILE_ENFORCED_OPTIONS PULP_PROFILE_NEUTRAL_OPTIONS)
    message(STATUS "fixture ${_option}=${${_option}}")
endforeach()
