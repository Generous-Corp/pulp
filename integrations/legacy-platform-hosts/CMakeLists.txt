# Legacy platform hosts: the Windows, Linux, iOS, Android and web UI hosts,
# kept as porting reference until each platform's Vellum host lands. See
# README.md.
#
# Nothing outside this directory may include, name or link what is here
# (tools/scripts/legacy_platform_hosts_guard.py). The Android and web hosts are
# compiled by tools/cmake/PulpAndroid.cmake, the root pulp-jni target and
# tools/cmake/PulpWebUi.cmake, which name their sources here directly. This
# file adds the Windows, Linux and iOS host sources and the quarantined
# headers' include roots to the core/view targets, under the same conditions
# core/view used for them, so every platform builds what it built before.

if(NOT TARGET pulp-view-core)
    return()
endif()

include(GNUInstallDirs)
set(_legacy_hosts "${CMAKE_CURRENT_SOURCE_DIR}")

# Public headers keep their pulp/view/... spelling. Every view target that
# exported core/view/include exports these too, and the installed SDK carries
# them at the same place.
foreach(_legacy_target pulp-view-core pulp-view-script pulp-view-tooling pulp-view
                       pulp-view-platform pulp-view-native)
    if(TARGET ${_legacy_target})
        target_include_directories(${_legacy_target} PUBLIC
            $<BUILD_INTERFACE:${_legacy_hosts}/win/include>
            $<BUILD_INTERFACE:${_legacy_hosts}/linux/include>
            $<BUILD_INTERFACE:${_legacy_hosts}/web/include>)
    endif()
endforeach()
unset(_legacy_target)
foreach(_legacy_platform win linux web)
    install(DIRECTORY "${_legacy_hosts}/${_legacy_platform}/include/pulp/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pulp"
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
endforeach()
unset(_legacy_platform)

# The pure UIA tree topology builder compiles everywhere, so its unit tests run
# on macOS and Linux. Provider ownership stays Windows-only.
target_sources(pulp-view-core PRIVATE
    ${_legacy_hosts}/win/src/accessibility_win_fragment_topology.cpp)

# core/view sets PULP_HAS_X11 on pulp-view-core when it found X11 for the
# Skia-backed Linux hosts.
get_target_property(_legacy_view_defs pulp-view-core COMPILE_DEFINITIONS)
if(NOT _legacy_view_defs)
    set(_legacy_view_defs "")
endif()

if(APPLE)
    if(PULP_IOS)
        target_sources(pulp-view-core PRIVATE
            ${_legacy_hosts}/ios/src/window_host_ios.mm
            ${_legacy_hosts}/ios/src/accessibility_ios.mm)
        target_sources(pulp-view-tooling PRIVATE
            ${_legacy_hosts}/ios/src/plugin_view_host_ios.mm)
    endif()
elseif(WIN32)
    target_sources(pulp-view-core PRIVATE
        ${_legacy_hosts}/win/src/text_accessibility_windows.cpp
        ${_legacy_hosts}/win/src/accessibility_win.cpp
        ${_legacy_hosts}/win/src/accessibility_win_events.cpp
        ${_legacy_hosts}/win/src/accessibility_win_fragment_lifecycle.cpp
        ${_legacy_hosts}/win/src/appearance_win.cpp
        ${_legacy_hosts}/win/src/motion_preferences_win.cpp
        # Standalone outbound file drag (free begin_file_drag via OLE). It has
        # no Skia or GPU dependency, so a non-GPU standalone still drags files.
        ${_legacy_hosts}/win/src/drag_drop_win.cpp)
    if(PULP_HAS_SKIA)
        target_sources(pulp-view-tooling PRIVATE
            ${_legacy_hosts}/win/src/plugin_view_host_win.cpp)
    endif()
    if(TARGET pulp-view-webview-backend)
        target_sources(pulp-view-webview-backend PRIVATE
            ${_legacy_hosts}/win/src/webview2_backend.cpp)
    endif()
elseif(UNIX AND NOT ANDROID)
    target_sources(pulp-view-core PRIVATE
        ${_legacy_hosts}/linux/src/text_accessibility_linux.cpp
        ${_legacy_hosts}/linux/src/accessibility_linux.cpp)
    if("PULP_HAS_X11=1" IN_LIST _legacy_view_defs)
        # Standalone outbound file drag (XDND source) and the X11 plug-in host.
        target_sources(pulp-view-core PRIVATE
            ${_legacy_hosts}/linux/src/drag_drop_linux.cpp)
        target_sources(pulp-view-tooling PRIVATE
            ${_legacy_hosts}/linux/src/plugin_view_host_linux.cpp)
    endif()
    if(TARGET pulp-view-webview-backend)
        target_sources(pulp-view-webview-backend PRIVATE
            ${_legacy_hosts}/linux/src/webkitgtk_backend.cpp)
    endif()
endif()

unset(_legacy_view_defs)
unset(_legacy_hosts)
