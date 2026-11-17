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

# The census itself, on fixed listings (cat stands in for nm): a std template
# instantiated over a pulp::view type is not pulp::view code, and a real
# pulp::view definition is.
find_program(_pulp_census_cat cat)
if(_pulp_census_cat)
    set(_census ${CMAKE_CURRENT_SOURCE_DIR}/check_symbol_census.cmake)
    set(_census_fixtures ${PROJECT_SOURCE_DIR}/test/fixtures/symbol_census)
    add_test(NAME symbol-census-ignores-std-instantiations
        COMMAND ${CMAKE_COMMAND} -DNM=${_pulp_census_cat}
            -DBINARY=${_census_fixtures}/std_instantiation_only.nm -DEXPECT=absent
            -DREQUIRED=clap_entry -P ${_census})
    add_test(NAME symbol-census-sees-a-view-definition
        COMMAND ${CMAKE_COMMAND} -DNM=${_pulp_census_cat}
            -DBINARY=${_census_fixtures}/view_definition.nm -DEXPECT=present -P ${_census})
    # Control: the same listing as absent must fail, or the census is blind.
    add_test(NAME symbol-census-rejects-a-view-definition
        COMMAND ${CMAKE_COMMAND} -DNM=${_pulp_census_cat}
            -DBINARY=${_census_fixtures}/view_definition.nm -DEXPECT=absent
            -DREQUIRED=clap_entry -P ${_census})
    set_tests_properties(symbol-census-ignores-std-instantiations
        symbol-census-sees-a-view-definition symbol-census-rejects-a-view-definition
        PROPERTIES LABELS "native-surface;cmake" TIMEOUT 60)
    set_tests_properties(symbol-census-rejects-a-view-definition PROPERTIES WILL_FAIL TRUE)
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

# The built plug-in loaded the way a host loads it, in this process only
# (nothing is installed): CLAP through clap_entry, AU v2 through
# AudioComponentRegister. Each processes a block and opens its GUI into a
# native surface. Plug-in editors refuse to open under CI and headless
# environment flags, so those are cleared for these cases.
if(PULP_BUILD_TESTS AND (TARGET PulpNativeMetalProbe_CLAP OR TARGET PulpNativeMetalProbe_AU))
    add_executable(pulp-test-native-metal-probe-host ${PROJECT_SOURCE_DIR}/test/native_metal_probe_hosts.mm)
    target_compile_options(pulp-test-native-metal-probe-host PRIVATE -fobjc-arc)
    target_link_libraries(pulp-test-native-metal-probe-host PRIVATE clap ${CMAKE_DL_LIBS}
        "-framework AppKit" "-framework AudioToolbox" "-framework AudioUnit")
    set(_probe_env
        "CI=unset:;PULP_HEADLESS=unset:;PULP_TEST_MODE=unset:;PULP_DISABLE_PLUGIN_EDITOR=unset:")
    if(TARGET PulpNativeMetalProbe_CLAP)
        add_dependencies(pulp-test-native-metal-probe-host PulpNativeMetalProbe_CLAP)
        add_test(NAME native-metal-probe-clap-host
            COMMAND pulp-test-native-metal-probe-host clap $<TARGET_FILE:PulpNativeMetalProbe_CLAP>)
        set_tests_properties(native-metal-probe-clap-host PROPERTIES
            LABELS "native-surface;editor-host;clap" TIMEOUT 60
            ENVIRONMENT_MODIFICATION "${_probe_env}")
    endif()
    if(TARGET PulpNativeMetalProbe_AU)
        add_dependencies(pulp-test-native-metal-probe-host PulpNativeMetalProbe_AU)
        add_test(NAME native-metal-probe-au-host
            COMMAND pulp-test-native-metal-probe-host au $<TARGET_BUNDLE_DIR:PulpNativeMetalProbe_AU>)
        set_tests_properties(native-metal-probe-au-host PROPERTIES
            LABELS "native-surface;editor-host;auv2" TIMEOUT 60
            ENVIRONMENT_MODIFICATION "${_probe_env}")
    endif()
endif()

# The probe's editor opened through each plug-in format's own adapter, in an
# offscreen window on the system Metal device: the AU v2 contract view, the
# VST3 contract view, the CLAP contract GUI and the macOS AU v3 view controller.
# Each asserts the same lifecycle (support/native_surface_format_checks.hpp): the
# editor attaches at its size, the first frame carries the gain colour at the
# drawable's size, a host resize round trip moves the drawable and returns, and
# a close then reopen starts clean. A test skips, saying so, without a Metal
# device.
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    include(Catch)
    function(_pulp_native_format_host_test name)
        cmake_parse_arguments(T "" "" "SOURCES;LIBRARIES;DEFINITIONS;LABEL" ${ARGN})
        add_executable(${name} ${T_SOURCES})
        # Only the test's own translation unit is ARC; the adapter sources
        # compiled into it keep their own memory management.
        list(GET T_SOURCES 0 _test_source)
        set_source_files_properties(${_test_source} PROPERTIES COMPILE_OPTIONS -fobjc-arc)
        target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test)
        target_link_libraries(${name} PRIVATE
            pulp-native-metal-probe-ui pulp::native Catch2::Catch2WithMain ${T_LIBRARIES}
            "-framework AppKit" "-framework ImageIO" "-framework Metal"
            "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudioKit")
        if(T_DEFINITIONS)
            target_compile_definitions(${name} PRIVATE ${T_DEFINITIONS})
        endif()
        set_target_properties(${name} PROPERTIES CXX_STANDARD 23)
        target_compile_options(${name} PRIVATE
            $<$<COMPILE_LANGUAGE:OBJCXX>:-std=gnu++23>)
        catch_discover_tests(${name}
            PROPERTIES LABELS "native-surface;editor-host;format-host;${T_LABEL}")
    endfunction()

    set(_fmt_src ${PROJECT_SOURCE_DIR}/core/format/src)
    if(TARGET pulp::format-clap-core AND TARGET clap)
        _pulp_native_format_host_test(pulp-test-native-surface-clap
            SOURCES ${PROJECT_SOURCE_DIR}/test/test_native_surface_clap.mm
            LIBRARIES pulp::format-clap-core clap
            LABEL clap)
    endif()
    if(PULP_HAS_VST3)
        _pulp_native_format_host_test(pulp-test-native-surface-vst3
            SOURCES ${PROJECT_SOURCE_DIR}/test/test_native_surface_vst3.mm
            LIBRARIES pulp::format-core
            LABEL vst3)
    endif()
    if(PULP_HAS_AUSDK)
        _pulp_native_format_host_test(pulp-test-native-surface-au-v2
            SOURCES ${PROJECT_SOURCE_DIR}/test/test_native_surface_au_v2.mm
                    ${_fmt_src}/au_v2_contract_view.mm
            LIBRARIES pulp::format-core ausdk
            DEFINITIONS
                PULP_AU_CONTRACT_VIEW_FACTORY_CLASS=PulpAUContractViewFactory_NativeSurface
                PULP_AU_CONTRACT_HOST_VIEW_CLASS=PulpAUContractHostView_NativeSurface
            LABEL auv2)
        if(NOT PULP_IOS AND TARGET pulp::view)
            _pulp_native_format_host_test(pulp-test-native-surface-au-v3
                SOURCES ${PROJECT_SOURCE_DIR}/test/test_native_surface_au_v3.mm
                        ${_fmt_src}/au_adapter.mm
                        ${_fmt_src}/au_view_controller_mac.mm
                        ${_fmt_src}/au_v3_contract_view.mm
                LIBRARIES pulp::format pulp::view ausdk
                LABEL auv3)
        endif()
    endif()
endif()
