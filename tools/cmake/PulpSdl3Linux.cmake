# SDL3 X11 extension defaults on Linux.
#
# Since 3.4, SDL3's configure stops with FATAL_ERROR when an X11 extension
# option it enables by default (Xcursor, XInput2, Xrandr, ...) has no
# development package installed, instead of quietly building without that
# extension as 3.2 did. A Linux host or CI image with libx11-dev but without
# every libx*-dev package therefore could not configure Pulp at all.
#
# pulp_sdl3_missing_x11_extensions() returns the SDL_X11_* options whose
# header is absent, so the caller can default exactly those OFF and keep the
# 3.2 behaviour: an extension is built when its package is present and skipped
# when it is not. An option the user already set is left alone, so
# -DSDL_X11_XCURSOR=ON on a host without the package still fails loudly in
# SDL's own check.

# Option name and the header SDL3 probes for it (cmake/sdlchecks.cmake).
set(_PULP_SDL3_X11_EXTENSION_HEADERS
    "SDL_X11_XCURSOR=X11/Xcursor/Xcursor.h"
    "SDL_X11_XDBE=X11/extensions/Xdbe.h"
    "SDL_X11_XINPUT=X11/extensions/XInput2.h"
    "SDL_X11_XFIXES=X11/extensions/Xfixes.h"
    "SDL_X11_XSYNC=X11/extensions/sync.h"
    "SDL_X11_XRANDR=X11/extensions/Xrandr.h"
    "SDL_X11_XSCRNSAVER=X11/extensions/scrnsaver.h"
    "SDL_X11_XSHAPE=X11/extensions/shape.h"
    "SDL_X11_XTEST=X11/extensions/XTest.h")

# pulp_sdl3_missing_x11_extensions(<out-var> [INCLUDE_DIRS <dir>...])
#
# Sets <out-var> to the SDL_X11_* options that are not yet defined and whose
# header cannot be found. INCLUDE_DIRS replaces the default system search
# (used by the test); without it the usual system include paths are searched.
function(pulp_sdl3_missing_x11_extensions out_var)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "" "INCLUDE_DIRS")
    set(_missing "")
    foreach(_entry IN LISTS _PULP_SDL3_X11_EXTENSION_HEADERS)
        string(REPLACE "=" ";" _pair "${_entry}")
        list(GET _pair 0 _option)
        list(GET _pair 1 _header)
        if(DEFINED ${_option})
            continue()
        endif()
        set(_found_var "_PULP_SDL3_HEADER_${_option}")
        unset(${_found_var} CACHE)
        unset(${_found_var})
        if(_arg_INCLUDE_DIRS)
            find_file(${_found_var} NAMES "${_header}"
                PATHS ${_arg_INCLUDE_DIRS} NO_DEFAULT_PATH NO_CACHE)
        else()
            find_file(${_found_var} NAMES "${_header}"
                PATHS /usr/include /usr/local/include /opt/X11/include NO_CACHE)
        endif()
        if(NOT ${_found_var})
            list(APPEND _missing "${_option}")
        endif()
    endforeach()
    set(${out_var} "${_missing}" PARENT_SCOPE)
endfunction()
