# On Linux the WebView (WebKitGTK) is opt-in per target. A plug-in on the
# default pulp::view stack must not link GTK or WebKit, because a desktop
# without WebKitGTK could not load it: its NEEDED list names no gtk, webkit or
# javascriptcore library, and its link closure reaches neither pulp-view-webview
# nor a GTK/WebKit import. A binary that does link pulp::view-webview is the
# positive control, so a scanner that sees nothing is blind rather than the
# plug-in clean.
if(NOT UNIX OR APPLE OR ANDROID OR NOT PULP_BUILD_WEBVIEW OR
   NOT TARGET PulpNoEditorLinkFloorControl_CLAP)
    return()
endif()

add_executable(pulp-linux-webview-control webview_control.cpp)
target_link_libraries(pulp-linux-webview-control PRIVATE pulp::view pulp::view-webview)

# Target-graph half, at configure time.
pulp_link_closure(PulpNoEditorLinkFloorControl_CLAP TARGETS_OUT _linux_webview_free_targets)
if(NOT "pulp-view" IN_LIST _linux_webview_free_targets AND
   NOT "pulp-format-view" IN_LIST _linux_webview_free_targets)
    message(FATAL_ERROR
        "the default-stack control no longer reaches the view stack, so the WebView-free check proves nothing: ${_linux_webview_free_targets}")
endif()
if("pulp-view-webview" IN_LIST _linux_webview_free_targets)
    message(FATAL_ERROR
        "a default-stack plug-in reaches pulp-view-webview: ${_linux_webview_free_targets}")
endif()
foreach(_reachable_target IN LISTS _linux_webview_free_targets)
    get_target_property(_direct_links "${_reachable_target}" LINK_LIBRARIES)
    get_target_property(_interface_links "${_reachable_target}" INTERFACE_LINK_LIBRARIES)
    if("${_direct_links};${_interface_links}" MATCHES "WEBKIT2GTK|GTK3")
        message(FATAL_ERROR
            "a default-stack plug-in reaches GTK/WebKit through ${_reachable_target}: ${_direct_links};${_interface_links}")
    endif()
endforeach()

add_test(NAME linux-webview-optional-artifacts
    COMMAND "${CMAKE_COMMAND}"
        -DPLUGIN=$<TARGET_FILE:PulpNoEditorLinkFloorControl_CLAP>
        -DWEBVIEW_CONTROL=$<TARGET_FILE:pulp-linux-webview-control>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scan_webview_optional.cmake")
set_tests_properties(linux-webview-optional-artifacts PROPERTIES
    LABELS "view;link-contract;artifact" TIMEOUT 30)
