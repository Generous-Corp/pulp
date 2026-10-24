#[[
Contract for pulp_sdl3_missing_x11_extensions() in
tools/cmake/PulpSdl3Linux.cmake. Runs under `cmake -P` on every platform.

SDL3 3.4+ stops configure on Linux when a default-on X11 extension has no
development header. The helper must report exactly the extensions whose header
is absent, and must never override an option the user already set.
]]

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_repo_root}/tools/cmake/PulpSdl3Linux.cmake")

set(_fail 0)
function(_expect label actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(SEND_ERROR "${label}: expected [${expected}], got [${actual}]")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

string(RANDOM LENGTH 8 _tag)
set(_root "${CMAKE_CURRENT_BINARY_DIR}/pulp-sdl3-x11-${_tag}")
file(REMOVE_RECURSE "${_root}")

# Only Xcursor and XInput2 headers exist; every other extension is missing.
file(WRITE "${_root}/X11/Xcursor/Xcursor.h" "")
file(WRITE "${_root}/X11/extensions/XInput2.h" "")
pulp_sdl3_missing_x11_extensions(_missing INCLUDE_DIRS "${_root}")
_expect("partial headers" "${_missing}"
    "SDL_X11_XDBE;SDL_X11_XFIXES;SDL_X11_XSYNC;SDL_X11_XRANDR;SDL_X11_XSCRNSAVER;SDL_X11_XSHAPE;SDL_X11_XTEST")

# A user-set option is never reported, even when its header is missing.
set(SDL_X11_XRANDR ON)
pulp_sdl3_missing_x11_extensions(_missing INCLUDE_DIRS "${_root}")
list(FIND _missing "SDL_X11_XRANDR" _idx)
_expect("user-set option left alone" "${_idx}" "-1")
unset(SDL_X11_XRANDR)

# Every header present: nothing is disabled.
foreach(_h Xdbe.h Xfixes.h sync.h Xrandr.h scrnsaver.h shape.h XTest.h)
    file(WRITE "${_root}/X11/extensions/${_h}" "")
endforeach()
pulp_sdl3_missing_x11_extensions(_missing INCLUDE_DIRS "${_root}")
_expect("all headers" "${_missing}" "")

# No headers at all: every extension is reported.
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}")
pulp_sdl3_missing_x11_extensions(_missing INCLUDE_DIRS "${_root}")
list(LENGTH _missing _count)
_expect("no headers" "${_count}" "9")

file(REMOVE_RECURSE "${_root}")
if(_fail)
    message(FATAL_ERROR "sdl3 linux x11 defaults: FAILED")
endif()
message(STATUS "sdl3 linux x11 defaults: OK")
