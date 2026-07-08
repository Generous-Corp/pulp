# PulpMinOs.cmake — pin the deployment target from tools/deps/min_os.json.
#
# Included BEFORE project() so CMAKE_OSX_DEPLOYMENT_TARGET is set before the
# compiler is configured (CMake requires it there). Pulp's macOS floor is the
# MAX minimum among everything it links: Skia and Dawn (prebuilt) plus libcxx
# (the toolchain C++ runtime — its std::format/std::to_chars(float) is gated at
# macOS 13.3 in the Apple SDK, which is what actually sets the floor above
# Google's 13.0 deps target). V8 counts only when PULP_JS_ENGINE=v8 (kept low, so
# it never raises the floor unless it is the highest).
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
  set(PULP_MIN_OS_JSON "${CMAKE_CURRENT_LIST_DIR}/../deps/min_os.json")
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

  # macos-arm64 is Pulp's only supported macOS target (ARM64-only; Intel unsupported).
  # skia + dawn are always linked; libcxx is the toolchain C++ runtime, whose
  # std::format/std::to_chars(float) availability sets the real floor above the
  # Google deps target. V8 counts only when it is the selected JS engine.
  set(_deps skia dawn libcxx)
  if(PULP_JS_ENGINE STREQUAL "v8")
    list(APPEND _deps v8)
  endif()

  set(_floor "")
  foreach(_dep IN LISTS _deps)
    string(JSON _m ERROR_VARIABLE _err GET "${_json}" platforms macos-arm64 deps ${_dep} measured)
    if(_err STREQUAL "NOTFOUND" AND _m AND NOT _m STREQUAL "null")
      if(_floor STREQUAL "" OR _m VERSION_GREATER _floor)
        set(_floor "${_m}")
      endif()
    endif()
  endforeach()

  if(_floor STREQUAL "")
    return()
  endif()

  # Decide whether to (re)pin per the override policy documented above.
  set(_apply FALSE)
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
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

# Detect the host robustly before project() (APPLE / CMAKE_HOST_APPLE are not yet
# reliably set at this point; uname is). uname is absent on Windows → empty → skip.
if(NOT DEFINED PULP_HOST_UNAME)
  execute_process(COMMAND uname -s
                  OUTPUT_VARIABLE PULP_HOST_UNAME
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()

if(PULP_HOST_UNAME STREQUAL "Darwin")
  _pulp_min_os_pin_macos()
endif()
