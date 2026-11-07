# metal-probe: the native-surface example plug-in (AU v2 and CLAP). Its editor
# is a UI package on pulp-native, so the plug-in links no Pulp view, canvas,
# render or GPU-runtime code, and no Skia, Dawn, Vellum or RmlUi.

add_library(pulp-native-metal-probe-ui STATIC metal_probe_ui.mm)
set_source_files_properties(metal_probe_ui.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_include_directories(pulp-native-metal-probe-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pulp-native-metal-probe-ui
    PUBLIC
        pulp::format-core
    PRIVATE
        pulp::native
        "-framework CoreGraphics"
        "-framework ImageIO"
        "-framework Metal")
set_target_properties(pulp-native-metal-probe-ui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PULP_EDITOR_FACTORY_FUNCTION "pulp_native_metal_probe::editor_factory"
    PULP_EDITOR_FACTORY_HEADER "metal_probe_ui.hpp")

set(_formats "")
if(PULP_HAS_CLAP)
    list(APPEND _formats CLAP)
endif()
if(PULP_HAS_AUSDK)
    list(APPEND _formats AU)
endif()
if(NOT _formats)
    return()
endif()

pulp_add_plugin(PulpNativeMetalProbe
    UI pulp-native-metal-probe-ui
    FORMATS           ${_formats}
    PLUGIN_NAME       "Pulp Native Metal Probe"
    BUNDLE_ID         "dev.pulp.native.metal-probe"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PNmp"
    MANUFACTURER_CODE "PlNt"
)

# Symbol census: the built plug-in carries no Skia, Dawn, wgpu, Vellum, RmlUi or
# pulp::view code. The control runs the same census on Pulp's view library,
# which must show pulp::view, so a blind listing cannot pass.
if(CMAKE_NM AND TARGET PulpNativeMetalProbe_CLAP)
    set(_census ${CMAKE_CURRENT_SOURCE_DIR}/check_symbol_census.cmake)
    add_test(NAME native-metal-probe-symbol-census
        COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
            -DBINARY=$<TARGET_FILE:PulpNativeMetalProbe_CLAP> -DEXPECT=absent
            -DREQUIRED=clap_entry
            -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/metal-probe-symbol-census.json
            -P ${_census})
    set_tests_properties(native-metal-probe-symbol-census PROPERTIES
        LABELS "native-surface;editor-host;cmake" TIMEOUT 120)
    if(TARGET PulpNativeMetalProbe_AU)
        add_test(NAME native-metal-probe-au-symbol-census
            COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
                -DBINARY=$<TARGET_FILE:PulpNativeMetalProbe_AU> -DEXPECT=absent
                -DREQUIRED=PulpNativeMetalProbeAUFactory
                -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/metal-probe-au-symbol-census.json
                -P ${_census})
        set_tests_properties(native-metal-probe-au-symbol-census PROPERTIES
            LABELS "native-surface;editor-host;cmake" TIMEOUT 120)
    endif()
    if(TARGET pulp-view-core)
        add_test(NAME native-metal-probe-symbol-census-control
            COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
                -DBINARY=$<TARGET_FILE:pulp-view-core> -DEXPECT=present -DREQUIRED=
                -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/view-core-symbol-census.json
                -P ${_census})
        set_tests_properties(native-metal-probe-symbol-census-control PROPERTIES
            LABELS "native-surface;editor-host;cmake" TIMEOUT 120)
    endif()
endif()

# The host proof: the probe editor driven through the editor-host contract (the
# path every format adapter takes), in offscreen windows on the system Metal
# device.
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-native-metal-probe ${PROJECT_SOURCE_DIR}/test/test_native_metal_probe.mm)
    target_compile_options(pulp-test-native-metal-probe PRIVATE -fobjc-arc)
    target_link_libraries(pulp-test-native-metal-probe PRIVATE
        pulp-native-metal-probe-ui pulp::native pulp::format-core Catch2::Catch2WithMain
        "-framework AppKit" "-framework ImageIO" "-framework Metal")
    include(Catch)
    catch_discover_tests(pulp-test-native-metal-probe
        PROPERTIES LABELS "native-surface;editor-host")
endif()
