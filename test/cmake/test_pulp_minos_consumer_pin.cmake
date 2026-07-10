#[[
Behavioral test: PulpMinOs.cmake pins the min-OS floor for a find_package(Pulp)
CONSUMER, i.e. when it runs AFTER the consumer's project() rather than before
Pulp's own.

The subtlety this guards: before project() the deployment target is genuinely
NOT DEFINED; after project() CMake has already DEFINED it as the empty string.
The consumer path must treat empty as "unset → pin", not fall through. If this
regresses, plugins built against the installed SDK silently inherit the build
host's OS floor instead of Pulp's.

Run standalone via `cmake -P`. Asserts the correct per-host outcome:
  * macOS   — CMAKE_OSX_DEPLOYMENT_TARGET pinned to the macos-arm64 floor
  * Windows — CMAKE_CXX_FLAGS carries _WIN32_WINNT from the windows-x64 floor
  * Linux   — PULP_LINUX_GLIBC_FLOOR exposed from the linux-x64 floor
]]

cmake_minimum_required(VERSION 3.24)

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PULP_MIN_OS_JSON "${_repo_root}/tools/deps/min_os.json")
if(NOT EXISTS "${PULP_MIN_OS_JSON}")
    message(FATAL_ERROR "min_os.json not found at ${PULP_MIN_OS_JSON}")
endif()
file(READ "${PULP_MIN_OS_JSON}" _json)

execute_process(COMMAND uname -s
                OUTPUT_VARIABLE _uname OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

if(_uname STREQUAL "Darwin")
    # Reproduce the post-project() consumer state: the target is a DEFINED-but-
    # empty CACHE entry (that is exactly what project() leaves when the developer
    # passed no -DCMAKE_OSX_DEPLOYMENT_TARGET). Using a cache entry — not a normal
    # variable — matters: PulpMinOs re-pins via `set(... CACHE ... FORCE)`, and a
    # normal variable of the same name would shadow that write and mask the fix.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "" CACHE STRING "simulated consumer (unset)" FORCE)
    set(CMAKE_OSX_SYSROOT "")

    # Expected floor = max(measured) over skia/dawn/libcxx (V8 only under v8 engine).
    set(_expected "")
    foreach(_dep skia dawn libcxx)
        string(JSON _m ERROR_VARIABLE _e GET "${_json}" platforms macos-arm64 deps ${_dep} measured)
        if(_e STREQUAL "NOTFOUND" AND _m AND NOT _m STREQUAL "null")
            if(_expected STREQUAL "" OR _m VERSION_GREATER _expected)
                set(_expected "${_m}")
            endif()
        endif()
    endforeach()

    include("${_repo_root}/tools/cmake/PulpMinOs.cmake")

    if(CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        message(FATAL_ERROR
            "Consumer pin regressed: CMAKE_OSX_DEPLOYMENT_TARGET is still empty "
            "after including PulpMinOs.cmake. A find_package(Pulp) consumer would "
            "ship a plugin targeting the build host's OS, not Pulp's floor.")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "${_expected}")
        message(FATAL_ERROR
            "Consumer pin wrong: expected ${_expected} (min_os.json macos-arm64 "
            "floor) but PulpMinOs pinned ${CMAKE_OSX_DEPLOYMENT_TARGET}.")
    endif()
    message(STATUS
        "min-OS consumer pin (macOS): empty target → ${CMAKE_OSX_DEPLOYMENT_TARGET} "
        "(floor ${_expected}). OK.")

elseif(CMAKE_HOST_WIN32)
    string(JSON _winnt ERROR_VARIABLE _e GET "${_json}" platforms windows-x64 win32_winnt)
    include("${_repo_root}/tools/cmake/PulpMinOs.cmake")
    if(NOT CMAKE_CXX_FLAGS MATCHES "_WIN32_WINNT")
        message(FATAL_ERROR
            "Consumer pin regressed (Windows): CMAKE_CXX_FLAGS has no _WIN32_WINNT "
            "after including PulpMinOs.cmake; a consumer would compile against the "
            "Windows SDK default API level, not Pulp's floor.")
    endif()
    message(STATUS "min-OS consumer pin (Windows): _WIN32_WINNT=${_winnt} present in flags. OK.")

else()
    # Linux (and any other host): no compile-time pin — the module exposes the
    # declared glibc floor for CI/packaging; the artifact is verified post-link.
    string(JSON _floor ERROR_VARIABLE _e GET "${_json}" platforms linux-x64 floor)
    include("${_repo_root}/tools/cmake/PulpMinOs.cmake")
    if(NOT DEFINED PULP_LINUX_GLIBC_FLOOR OR PULP_LINUX_GLIBC_FLOOR STREQUAL "")
        message(FATAL_ERROR
            "Consumer note regressed (Linux): PULP_LINUX_GLIBC_FLOOR not exposed "
            "after including PulpMinOs.cmake.")
    endif()
    if(NOT PULP_LINUX_GLIBC_FLOOR STREQUAL "${_floor}")
        message(FATAL_ERROR
            "Linux floor wrong: expected ${_floor} but got ${PULP_LINUX_GLIBC_FLOOR}.")
    endif()
    message(STATUS "min-OS consumer note (Linux): glibc floor ${PULP_LINUX_GLIBC_FLOOR}. OK.")
endif()
