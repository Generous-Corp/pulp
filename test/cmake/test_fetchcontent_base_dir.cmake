#[[
Smoke test for the Windows FetchContent base-dir guard in
tools/cmake/PulpFetchContent.cmake.

The Windows WebGPU prebuilt path can exceed MSBuild's MAX_PATH limit when
FetchContent leaves nested subbuilds under a deep Actions workspace. The guard
keeps transient FetchContent build trees in a short per-user cache directory
while still respecting explicit operator overrides.
]]

cmake_minimum_required(VERSION 3.20)

set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(_repo_root "${_repo_root}" ABSOLUTE)

file(READ "${_repo_root}/tools/cmake/PulpDependencies.cmake" _deps_cmake)
set(_helper_include_line "include($\{CMAKE_CURRENT_SOURCE_DIR\}/tools/cmake/PulpFetchContent.cmake)")
string(FIND "${_deps_cmake}" "${_helper_include_line}" _helper_include)
string(FIND "${_deps_cmake}" "include(FetchContent)" _fetchcontent_include)
if(_helper_include LESS 0 OR _fetchcontent_include LESS 0
   OR NOT _helper_include LESS _fetchcontent_include)
    message(FATAL_ERROR
        "PulpFetchContent.cmake must be included before include(FetchContent) "
        "so FETCHCONTENT_BASE_DIR is configured before CMake initializes "
        "FetchContent's default _deps path.")
endif()

# Simulate the Windows branch without requiring a Windows host.
set(WIN32 TRUE)

set(ENV{PULP_FETCHCONTENT_BASE_DIR} "")
set(ENV{LOCALAPPDATA} "C:/Users/test/AppData/Local")
unset(FETCHCONTENT_BASE_DIR CACHE)
unset(FETCHCONTENT_BASE_DIR)
unset(PULP_FETCHCONTENT_AUTOMATIC_BASE_DIR CACHE)
set(CMAKE_BINARY_DIR "C:/builds/pulp-native-tools")

include("${_repo_root}/tools/cmake/PulpFetchContent.cmake")

set(_short_root "C:/Users/test/AppData/Local/Pulp/fc")
if(NOT FETCHCONTENT_BASE_DIR MATCHES "^${_short_root}/[0-9a-f]+$")
    message(FATAL_ERROR
        "Expected an isolated short FetchContent base under '${_short_root}', got "
        "'${FETCHCONTENT_BASE_DIR}'.")
endif()
set(_native_base "${FETCHCONTENT_BASE_DIR}")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL _native_base)
    message(FATAL_ERROR "Reconfiguring one build must retain its binary cache")
endif()
set(CMAKE_BINARY_DIR "C:/BUILDS/pulp-native-tools/../pulp-native-tools/")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL _native_base)
    message(FATAL_ERROR "Equivalent Windows build paths selected different caches")
endif()

# A separate ARM64EC build must not reuse native ARM64 objects, even when the
# immutable shared-source cache and the Windows desktop account are the same.
set(CMAKE_BINARY_DIR "C:/builds/pulp-arm64ec-trace")
pulp_configure_fetchcontent_base_dir()
if(FETCHCONTENT_BASE_DIR STREQUAL _native_base)
    message(FATAL_ERROR "Distinct build roots still share dependency binaries")
endif()
set(_ec_base "${FETCHCONTENT_BASE_DIR}")
unset(FETCHCONTENT_BASE_DIR CACHE)
unset(PULP_FETCHCONTENT_AUTOMATIC_BASE_DIR CACHE)
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL _ec_base)
    message(FATAL_ERROR "A clean configure of the same build must select the same cache")
endif()

# Legacy automatic entries are migrated without deleting their old contents.
unset(PULP_FETCHCONTENT_AUTOMATIC_BASE_DIR CACHE)
set(FETCHCONTENT_BASE_DIR "${_short_root}" CACHE PATH
    "Base directory for FetchContent dependency subbuilds" FORCE)
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL _ec_base)
    message(FATAL_ERROR "Legacy automatic cache did not migrate to the build-specific path")
endif()

unset(FETCHCONTENT_BASE_DIR CACHE)
unset(FETCHCONTENT_BASE_DIR)
set(ENV{PULP_FETCHCONTENT_BASE_DIR} "D:/pulp/fc")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL "D:/pulp/fc")
    message(FATAL_ERROR
        "Expected PULP_FETCHCONTENT_BASE_DIR override, got "
        "'${FETCHCONTENT_BASE_DIR}'.")
endif()

set(FETCHCONTENT_BASE_DIR "E:/operator/fc" CACHE PATH "" FORCE)
set(ENV{PULP_FETCHCONTENT_BASE_DIR} "D:/ignored/fc")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL "E:/operator/fc")
    message(FATAL_ERROR
        "Expected existing FETCHCONTENT_BASE_DIR to win, got "
        "'${FETCHCONTENT_BASE_DIR}'.")
endif()

# Explicit use of even the old shared root remains explicit on reconfigure.
unset(FETCHCONTENT_BASE_DIR CACHE)
set(ENV{PULP_FETCHCONTENT_BASE_DIR} "${_short_root}")
pulp_configure_fetchcontent_base_dir()
set(ENV{PULP_FETCHCONTENT_BASE_DIR} "")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL _short_root)
    message(FATAL_ERROR "Explicit legacy-root override was mistaken for an automatic entry")
endif()

# Source-cache sharing is independent of binary subbuild placement.
set(ENV{PULP_SHARED_FETCHCONTENT_SOURCE_DIR} "D:/shared/immutable-sources")
pulp_default_fetchcontent_cache_root(_source_root)
if(NOT _source_root STREQUAL "D:/shared/immutable-sources")
    message(FATAL_ERROR "Binary isolation changed the immutable shared-source override")
endif()

message(STATUS "FetchContent Windows base-dir guard: all cases pass.")
