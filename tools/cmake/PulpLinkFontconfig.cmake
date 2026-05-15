# PulpLinkFontconfig.cmake — link-order fix for libskia.a → fontconfig
# on Linux + GNU ld.
#
# Pulp #1962 / #1986 root cause: `libskia.a` (pulled in by
# `pulp::canvas`'s PUBLIC `skia::skia` dep) contains
# `SkFontMgr_New_FontConfig` which references `FcInit*` / `FcConfig*` /
# `FcPattern*` / `FcGetVersion` / `FcFontSet*` etc. GNU ld processes
# static archives left-to-right and only pulls in symbols that are
# unresolved AT THE TIME the archive is processed; symbols referenced
# AFTER the archive go unresolved even if a later library would have
# satisfied them. Re-mentioning `fontconfig` here ensures it appears
# AFTER `libskia.a` in the final link line.
#
# `PkgConfig::FONTCONFIG` is already wired PUBLIC on `pulp-canvas` per
# pulp #1962 follow-up, so the include dirs and -L flags are already
# correct — this helper just nails the link-line position down for
# every downstream binary that pulls in pulp::view / pulp::canvas.
#
# Originally inlined in tools/cli/CMakeLists.txt (#1986). Extracted
# here so the same fix can apply to pulp-import-design, pulp-design-debug,
# pulp-screenshot, and any future Linux+Skia binary without copy-paste
# drift. Pulp #1986 follow-up — also needed for v0.100.0 release-cli
# linux-x64 build of pulp-import-design.
#
# Usage:
#     include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake)
#     pulp_link_fontconfig_after_skia(my-target)
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
