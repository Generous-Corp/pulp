# PulpMinOs.cmake — pin the deployment target from tools/deps/min_os.json.
#
# Included BEFORE project() so CMAKE_OSX_DEPLOYMENT_TARGET is set before the
# compiler is configured (CMake requires it there). Pulp's macOS floor is the
# MAX minimum among everything it links: Skia and Dawn (prebuilt) plus libcxx
# (the toolchain C++ runtime — the macOS 15.4 SDK gates the floating-point
# std::to_chars overloads reached through std::format at macOS 13.4, which is
# what actually sets the floor above Google's 13.0 deps target). V8 counts only
# when PULP_JS_ENGINE=v8 (kept low, so it never raises the floor unless it is the
# highest).
#
# The numbers come from tools/deps/min_os.json (regenerate with
# tools/scripts/measure_min_os.py after any Skia/Dawn/V8 pin bump). This replaces
# the previous behavior where the macOS deployment target was UNPINNED and drifted
# with the build machine's SDK.
#
# Override policy (a deployment target is only a compile flag, so the rules are
# about what actually builds):
#   * unset                    → pin to the floor
#   * set BELOW the floor       → raise to the floor (a below-floor target does not
#                                 compile: stale warm-dir cache, or a mistaken -D)
#   * equal to our own sentinel → re-pin to the floor (so a bump — up OR down — is
#                                 followed on a warm reconfigure)
#   * set AT/ABOVE the floor by
#     someone else              → respect it (you may raise the minimum, e.g. to
#                                 build for a newer OS; you may not lower it below
#                                 what the toolchain/deps support)

if(NOT DEFINED PULP_MIN_OS_JSON)
  # Two layouts carry this module:
  #   * source tree  — tools/cmake/PulpMinOs.cmake → tools/deps/min_os.json
  #   * installed SDK — lib/cmake/Pulp/PulpMinOs.cmake → lib/cmake/Pulp/min_os.json
  #     (PulpInstallRules.cmake ships min_os.json next to this module so a
  #     find_package(Pulp) consumer inherits the same floor as Pulp itself).
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../deps/min_os.json")
    set(PULP_MIN_OS_JSON "${CMAKE_CURRENT_LIST_DIR}/../deps/min_os.json")
  else()
    set(PULP_MIN_OS_JSON "${CMAKE_CURRENT_LIST_DIR}/min_os.json")
  endif()
endif()

# A min_os.json edit is a data change CMake would not otherwise treat as a
# configure trigger, so a warm build dir would keep a stale deployment target
# after a pin bump. Make the file a configure dependency so any bump re-runs
# configure and the pin is recomputed.
if(EXISTS "${PULP_MIN_OS_JSON}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PULP_MIN_OS_JSON}")
endif()

function(_pulp_min_os_pin_macos)
  # An Apple toolchain file (ios.toolchain.cmake, etc.) targets a non-macOS SDK;
  # the macOS floor does not apply. Match those sysroots POSITIVELY — an empty or
  # macOS sysroot (including the plain cached "" on a warm reconfigure) must fall
  # through, so "NOT matches mac" is wrong here.
  if(CMAKE_OSX_SYSROOT MATCHES "(iphone|appletv|watch|xros|xrsimulator|visionos)")
    return()
  endif()
  if(NOT EXISTS "${PULP_MIN_OS_JSON}")
    return()
  endif()
  file(READ "${PULP_MIN_OS_JSON}" _json)

  # skia + dawn are always linked; libcxx is the toolchain C++ runtime, whose
  # std::format/floating-point std::to_chars availability sets the real floor
  # above the Google deps target. V8 counts only when it is the selected JS
  # engine.
  set(_deps skia dawn libcxx)
  if(PULP_JS_ENGINE STREQUAL "v8")
    list(APPEND _deps v8)
  endif()

  # Map the requested TARGET arches to min_os.json platform keys. Pulp now
  # supports arm64, x86_64, AND universal on macOS, so the floor is NOT a fixed
  # "macos-arm64" lookup — it is the MAX floor across every requested arch
  # (each arch's prebuilts may stamp a different minos; the final binary must
  # satisfy the highest). CMAKE_OSX_ARCHITECTURES unset ⇒ host arch.
  set(_req_archs "${CMAKE_OSX_ARCHITECTURES}")
  if(_req_archs STREQUAL "")
    execute_process(COMMAND uname -m
                    OUTPUT_VARIABLE _host_machine
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_host_machine MATCHES "arm64|aarch64")
      set(_req_archs "arm64")
    else()
      set(_req_archs "x86_64")
    endif()
  endif()
  set(_plat_keys "")
  if(_req_archs MATCHES "arm64")
    list(APPEND _plat_keys "macos-arm64")
  endif()
  if(_req_archs MATCHES "x86_64")
    list(APPEND _plat_keys "macos-x64")
  endif()
  if(_plat_keys STREQUAL "")
    return()
  endif()

  set(_floor "")
  foreach(_plat IN LISTS _plat_keys)
    foreach(_dep IN LISTS _deps)
      string(JSON _m ERROR_VARIABLE _err GET "${_json}" platforms ${_plat} deps ${_dep} measured)
      if(_err STREQUAL "NOTFOUND" AND _m AND NOT _m STREQUAL "null")
        if(_floor STREQUAL "" OR _m VERSION_GREATER _floor)
          set(_floor "${_m}")
        endif()
      endif()
    endforeach()
  endforeach()

  if(_floor STREQUAL "")
    return()
  endif()

  # Decide whether to (re)pin per the override policy documented above.
  #
  # "unset" has two forms depending on when this runs. Included BEFORE project()
  # (Pulp's own build) the variable is genuinely NOT DEFINED. Included from
  # PulpConfig.cmake at find_package() time (an installed-SDK consumer) it runs
  # AFTER the consumer's project(), where CMake has already DEFINED it as the
  # empty string. Both mean "the developer did not choose a target" → pin
  # silently to the floor; only a non-empty value BELOW the floor is a real
  # (warn-worthy) below-floor request.
  set(_apply FALSE)
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(_apply TRUE)
  elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS _floor)
    set(_apply TRUE)
    message(WARNING "Pulp min-OS: requested macOS deployment target "
      "${CMAKE_OSX_DEPLOYMENT_TARGET} is below the floor ${_floor}; raising it. "
      "The linked prebuilts/toolchain do not support the lower value.")
  elseif(DEFINED PULP_MIN_OS_PINNED AND
         CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL PULP_MIN_OS_PINNED)
    set(_apply TRUE)  # our own prior pin — follow the floor even if it moved down
  endif()

  if(_apply)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_floor}" CACHE STRING
        "Minimum macOS: max of linked dependency minima (tools/deps/min_os.json)" FORCE)
    set(PULP_MIN_OS_PINNED "${_floor}" CACHE INTERNAL
        "Deployment target this module pinned; lets a warm reconfigure re-pin after a min_os.json bump")
    message(STATUS "Pulp min-OS: macOS deployment target = ${_floor} (max of: ${_deps})")
  endif()
endfunction()

function(_pulp_min_os_pin_windows)
  # Windows has no "deployment target"; the floor is the compile-time
  # _WIN32_WINNT / WINVER API level. Pulp does not set it, so it otherwise
  # inherits the Windows SDK default — a newer SDK can silently raise the floor
  # (the Windows analog of the macOS Dawn-minos leak). Pin it to min_os.json's
  # windows-x64 floor (Windows 10 = 0x0A00 at the current Skia/Dawn milestone).
  if(NOT EXISTS "${PULP_MIN_OS_JSON}")
    return()
  endif()
  file(READ "${PULP_MIN_OS_JSON}" _json)
  string(JSON _winnt ERROR_VARIABLE _err GET "${_json}" platforms windows-x64 win32_winnt)
  if(NOT _err STREQUAL "NOTFOUND" OR _winnt STREQUAL "" OR _winnt STREQUAL "null")
    return()
  endif()

  # Respect a _WIN32_WINNT the developer (or a toolchain file) already set, and
  # stay idempotent on a warm reconfigure — both show up as an existing match in
  # the flags. -D is accepted by MSVC (cl), clang-cl, and MinGW alike.
  if(CMAKE_CXX_FLAGS MATCHES "_WIN32_WINNT")
    return()
  endif()

  set(_defs "-D_WIN32_WINNT=${_winnt} -DWINVER=${_winnt}")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_defs}"   CACHE STRING "Pulp min-OS: Windows API floor" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_defs}" CACHE STRING "Pulp min-OS: Windows API floor" FORCE)
  message(STATUS "Pulp min-OS: Windows _WIN32_WINNT/WINVER pinned to ${_winnt} (min_os.json windows-x64 floor)")
endfunction()

function(_pulp_min_os_note_linux)
  # Linux has no compile-time deployment target to pin (unlike the macOS
  # -mmacosx-version-min or the Windows _WIN32_WINNT define). The glibc floor of
  # a final binary is a POST-LINK property: max(the prebuilts' floor, the floor
  # the *build host's* glibc leaks into Pulp's own compiled objects). So there is
  # nothing to force here — only a value to expose and a check to run after the
  # link. We publish the declared floor as PULP_LINUX_GLIBC_FLOOR (for CI /
  # packaging) and leave the actual verification to:
  #   tools/scripts/measure_min_os.py --elf <built .so/exe> --max <floor>
  # run over a freshly linked Pulp binary. The shipped Skia/Dawn/V8 prebuilts are
  # already built on an Ubuntu 22.04 base (glibc 2.34) by the portable-linux
  # release lanes, so a floor above 2.34 means THIS build host re-leaked a newer
  # glibc — build Pulp's Linux artifacts on the same <=floor base.
  if(NOT EXISTS "${PULP_MIN_OS_JSON}")
    return()
  endif()
  file(READ "${PULP_MIN_OS_JSON}" _json)
  string(JSON _floor ERROR_VARIABLE _err GET "${_json}" platforms linux-x64 floor)
  if(NOT _err STREQUAL "NOTFOUND" OR _floor STREQUAL "" OR _floor STREQUAL "null")
    return()
  endif()
  set(PULP_LINUX_GLIBC_FLOOR "${_floor}" CACHE STRING
      "Pulp min-OS: declared Linux glibc floor (verify built binaries with measure_min_os.py --elf)")
  message(STATUS "Pulp min-OS: Linux glibc floor is ${_floor} "
                 "(post-link check: measure_min_os.py --elf <binary> --max ${_floor})")
endfunction()

# Detect the host robustly before project() (APPLE / CMAKE_HOST_APPLE are not yet
# reliably set at this point; uname is). uname is absent on Windows → empty, so
# CMAKE_HOST_WIN32 (set before project()) is the Windows signal.
if(NOT DEFINED PULP_HOST_UNAME)
  execute_process(COMMAND uname -s
                  OUTPUT_VARIABLE PULP_HOST_UNAME
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()

if(PULP_HOST_UNAME STREQUAL "Darwin")
  _pulp_min_os_pin_macos()
elseif(CMAKE_HOST_WIN32)
  _pulp_min_os_pin_windows()
elseif(PULP_HOST_UNAME STREQUAL "Linux")
  _pulp_min_os_note_linux()
endif()
