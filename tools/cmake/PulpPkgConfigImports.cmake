# PulpPkgConfigImports.cmake — recreate Linux `PkgConfig::*` IMPORTED
# targets for downstream find_package(Pulp).
#
# Extracted from PulpConfig.cmake.in in the 2026-05 B3 refactor.
#
# core/view + core/canvas link several `PkgConfig::*` imported
# targets on Linux when their dev headers are present. Those target
# names are baked into PulpTargets.cmake's link interfaces, so
# downstream `find_package(Pulp)` must re-create them via the same
# pkg_check_modules invocations — otherwise CMake aborts at
# consumer-configure time with:
#     The link interface of target "Pulp::view" contains:
#       PkgConfig::<name>
#     but the target was not found.
# The smoke workflow at .github/workflows/install-consumer-smoke.yml
# catches this on every PR (pulp #2087 follow-up). QUIET probes match
# the in-tree builds — a consumer without one of the dev headers
# degrades the same way the build does (no compile-time PULP_HAS_*
# define, no link-interface entry).
#
# Mirror is structural: any new `pkg_check_modules(... IMPORTED_TARGET
# ...)` in core/view or core/canvas needs a matching line here. The
# smoke workflow is the authoritative gate; if it fails on a missing
# PkgConfig::FOO downstream, add `pkg_check_modules(FOO IMPORTED_TARGET
# <mod>)` below.

if(NOT PULP_LINUX)
    return()
endif()

find_dependency(ALSA)
find_dependency(PkgConfig QUIET)
if(PkgConfig_FOUND)
    # core/canvas: fontconfig — m144 Skia uses it for font enumeration.
    pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
    # core/view accessibility_linux.cpp — AtkRole + AtkObject bridge.
    pkg_check_modules(PULP_ATK QUIET IMPORTED_TARGET atk)
    pkg_check_modules(ATK_BRIDGE QUIET IMPORTED_TARGET atk-bridge-2.0)
    # core/view WebView (PULP_BUILD_WEBVIEW=ON) — only relevant when
    # the SDK was built with WebView. Probe with QUIET; if the
    # consumer doesn't have the libs, find_package degrades the
    # same way the SDK build would.
    pkg_check_modules(GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(WEBKIT2GTK QUIET IMPORTED_TARGET webkit2gtk-4.1)
endif()
