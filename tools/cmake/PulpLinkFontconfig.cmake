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

# pulp_link_fontconfig_needed(<module-target>)
#
# For a Linux plug-in module (VST3 / CLAP / LV2 / AAX) whose editor links Skia.
# `libskia.a` references the `Fc*` functions, and a MODULE library may leave
# symbols undefined, so the link succeeds without naming libfontconfig. The
# module then loads only in a host that already has fontconfig in its process
# (REAPER and every GTK host do); any other host fails at dlopen under
# RTLD_NOW, or at the first font lookup.
#
# This records libfontconfig as a DT_NEEDED entry. `--no-as-needed` keeps
# Ubuntu's default `--as-needed` from dropping it: at the point the library
# appears on the line nothing yet references it, so it would be discarded.
# FreeType needs no entry of its own because libfontconfig depends on it, so it
# is already in the module's dependency tree. If fontconfig is not installed
# the module is left as it was and a warning names the missing package.
#
# No-op on non-Linux platforms.
function(pulp_link_fontconfig_needed target)
    if(NOT (UNIX AND NOT APPLE AND NOT ANDROID))
        return()
    endif()
    find_library(PULP_FONTCONFIG_LIBRARY NAMES fontconfig)
    if(NOT PULP_FONTCONFIG_LIBRARY)
        message(WARNING
            "${target}: libfontconfig was not found, so this plug-in will not "
            "name it as a needed library and loads only in hosts that already "
            "have it (install libfontconfig-dev).")
        return()
    endif()
    target_link_libraries(${target} PRIVATE
        "-Wl,--push-state,--no-as-needed"
        "${PULP_FONTCONFIG_LIBRARY}"
        "-Wl,--pop-state")
endfunction()
