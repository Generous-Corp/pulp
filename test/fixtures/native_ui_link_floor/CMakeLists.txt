if(NOT APPLE OR PULP_IOS OR NOT PULP_HAS_SKIA OR NOT PULP_BUILD_WEBVIEW OR
   NOT PULP_ENABLE_JS OR NOT PULP_HAS_CLAP)
    return()
endif()

pulp_add_plugin(PulpNativeLinkFloor
    NATIVE_UI
    FORMATS CLAP Standalone
    SOURCES native_ui_link_floor.cpp
    PROCESSOR_FACTORY make_native_ui_link_floor
    PLUGIN_NAME "Pulp Native Link Floor"
    BUNDLE_ID "dev.pulp.test.native-link-floor"
    MANUFACTURER "Pulp"
    VERSION "1.0.0"
    CATEGORY Effect)

option(PULP_NATIVE_LINK_FLOOR_MUTANT
    "Test-only mutation: contaminate the native fixture with the legacy view target"
    OFF)
if(PULP_NATIVE_LINK_FLOOR_MUTANT)
    target_link_libraries(PulpNativeLinkFloor_CLAP PRIVATE pulp::view)
endif()

# A binary-level positive control proves the scanner can actually observe
# WebKit. Without this, a broken otool invocation would make the absence check
# green for every artifact.
add_executable(pulp-native-link-floor-webkit-control webkit_control.cpp)
target_link_libraries(pulp-native-link-floor-webkit-control PRIVATE pulp::view)

# Target-graph contract: mutating NATIVE_UI back to the legacy view umbrella,
# or moving WebKit onto a native dependency, fails at configure time before
# dead stripping can conceal the regression in the final binary.
pulp_link_closure(PulpNativeLinkFloor_CLAP
    TARGETS_OUT _native_link_floor_targets)
foreach(_forbidden_target pulp-view pulp-view-webview pulp-standalone)
    if(_forbidden_target IN_LIST _native_link_floor_targets)
        message(FATAL_ERROR
            "PulpNativeLinkFloor_CLAP reaches forbidden target ${_forbidden_target}: ${_native_link_floor_targets}")
    endif()
endforeach()
foreach(_required_target pulp-view-native pulp-view-script pulp-view-core pulp-render)
    if(NOT _required_target IN_LIST _native_link_floor_targets)
        message(FATAL_ERROR
            "PulpNativeLinkFloor_CLAP lost required target ${_required_target}: ${_native_link_floor_targets}")
    endif()
endforeach()
foreach(_reachable_target IN LISTS _native_link_floor_targets)
    get_target_property(_direct_links "${_reachable_target}" LINK_LIBRARIES)
    get_target_property(_interface_links "${_reachable_target}" INTERFACE_LINK_LIBRARIES)
    if("${_direct_links};${_interface_links}" MATCHES "WebKit|WEBKIT2GTK")
        message(FATAL_ERROR
            "PulpNativeLinkFloor_CLAP reaches WebView linkage through ${_reachable_target}: ${_direct_links};${_interface_links}")
    endif()
endforeach()

set(_native_link_floor_scan "${CMAKE_CURRENT_SOURCE_DIR}/scan_native_link_floor.cmake")
add_custom_target(pulp-native-link-floor-proof
    COMMAND "${CMAKE_COMMAND}"
        -DNATIVE_STANDALONE=$<TARGET_FILE:PulpNativeLinkFloor_Standalone>
        -DNATIVE_CLAP=$<TARGET_FILE:PulpNativeLinkFloor_CLAP>
        -DPOSITIVE_CONTROL=$<TARGET_FILE:pulp-native-link-floor-webkit-control>
        -P "${_native_link_floor_scan}"
    DEPENDS
        PulpNativeLinkFloor_Standalone
        PulpNativeLinkFloor_CLAP
        pulp-native-link-floor-webkit-control
    VERBATIM)

add_test(NAME native-ui-link-floor-artifacts
    COMMAND "${CMAKE_COMMAND}"
        -DNATIVE_STANDALONE=$<TARGET_FILE:PulpNativeLinkFloor_Standalone>
        -DNATIVE_CLAP=$<TARGET_FILE:PulpNativeLinkFloor_CLAP>
        -DPOSITIVE_CONTROL=$<TARGET_FILE:pulp-native-link-floor-webkit-control>
        -P "${_native_link_floor_scan}")
set_tests_properties(native-ui-link-floor-artifacts PROPERTIES
    LABELS "view;link-contract;artifact" TIMEOUT 30)
