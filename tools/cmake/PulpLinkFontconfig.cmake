# PulpLinkFontconfig.cmake — link-order fix for libskia.a → fontconfig
# on Linux + GNU ld.
#
# Root cause: `libskia.a` (pulled in by
# `pulp::canvas`'s PUBLIC `skia::skia` dep) contains
# `SkFontMgr_New_FontConfig` which references `FcInit*` / `FcConfig*` /
# `FcPattern*` / `FcGetVersion` / `FcFontSet*` etc. GNU ld processes
# static archives left-to-right and only pulls in symbols that are
# unresolved AT THE TIME the archive is processed; symbols referenced
# AFTER the archive go unresolved even if a later library would have
# satisfied them. Re-mentioning `fontconfig` here ensures it appears
# AFTER `libskia.a` in the final link line.
#
# `PkgConfig::FONTCONFIG` is already wired PUBLIC on `pulp-canvas`, so
# the include dirs and -L flags are already
# correct — this helper just nails the link-line position down for
# every downstream binary that pulls in pulp::view / pulp::canvas.
#
# Originally inlined in tools/cli/CMakeLists.txt (#1986). Extracted
# here so the same fix can apply to pulp-import-design, pulp-design-debug,
# pulp-screenshot, release-cli linux-x64 builds, and any future Linux+Skia
# binary without copy-paste drift.
#
# Usage (in-tree Pulp build):
#     include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake")
#     pulp_link_fontconfig_after_skia(my-target)
#
# Usage (consumed from installed SDK via find_package(Pulp)):
#     This file ships alongside PulpUtils.cmake at lib/cmake/Pulp/; the
#     helper functions in PulpUtils.cmake include it via CMAKE_CURRENT_LIST_DIR
#     so downstream consumers don't need to reference it directly.
#
# No-op on non-Linux platforms (macOS uses CoreText, Windows uses
# DirectWrite, Android bundles its own font manager).
function(pulp_link_fontconfig_after_skia target)
    if(NOT (UNIX AND NOT APPLE AND NOT ANDROID))
        return()
    endif()
    find_package(PkgConfig)
    if(NOT PkgConfig_FOUND)
        return()
    endif()
    # Use a target-scoped variable name so multiple callers don't collide.
    set(_var "_PULP_${target}_FONTCONFIG")
    pkg_check_modules(${_var} IMPORTED_TARGET fontconfig)
    if(${_var}_FOUND)
        target_link_libraries(${target} PRIVATE PkgConfig::${_var})
    endif()
endfunction()

# pulp_fontconfig_needed_on_target(<target> <fontconfig-library>)
#
# Appends libfontconfig, as a needed library, to <target>'s INTERFACE link
# line. The installed package config applies it to Pulp::canvas AFTER the
# Skia archive is attached, for a consumer that links Pulp's static view or
# canvas libraries directly (not through pulp_add_plugin) into a plug-in module.
#
# `libskia.a` references the `Fc*` functions, and a MODULE library may leave
# symbols undefined, so such a link succeeds without naming libfontconfig. The
# module then loads only in a host that already has fontconfig in its process
# (REAPER and every GTK host do); any other host fails at dlopen under
# RTLD_NOW, or at the first font lookup. `--no-as-needed` keeps Ubuntu's
# default `--as-needed` from discarding the library, which at that point has
# no reference to satisfy. FreeType needs no entry of its own because
# libfontconfig depends on it.
function(pulp_fontconfig_needed_on_target target fontconfig_library)
    set_property(TARGET ${target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES
        "-Wl,--push-state,--no-as-needed"
        "${fontconfig_library}"
        "-Wl,--pop-state")
endfunction()

# pulp_attach_fontconfig_to_canvas(<canvas-target>)
#
# Linux only (no-op elsewhere): finds libfontconfig and applies
# pulp_fontconfig_needed_on_target. Without the library it warns and leaves the
# target as it was.
function(pulp_attach_fontconfig_to_canvas target)
    if(NOT (UNIX AND NOT APPLE AND NOT ANDROID))
        return()
    endif()
    find_library(PULP_FONTCONFIG_LIBRARY NAMES fontconfig)
    if(NOT PULP_FONTCONFIG_LIBRARY)
        message(WARNING
            "libfontconfig was not found, so ${target} will not name it as a "
            "needed library and a plug-in linking it loads only in hosts that "
            "already have fontconfig (install libfontconfig-dev).")
        return()
    endif()
    pulp_fontconfig_needed_on_target(${target} "${PULP_FONTCONFIG_LIBRARY}")
endfunction()
