set(PULP_PHASE_F_PREVIEW_GENERATED_DIR
    "${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated")
set(PULP_PHASE_F_PREVIEW_GENERATED_CPP
    "${PULP_PHASE_F_PREVIEW_GENERATED_DIR}/chainer-phase-f-hybrid.cpp")

add_executable(pulp-native-ui-phase-f-preview MACOSX_BUNDLE
    main.cpp
    "${PULP_PHASE_F_PREVIEW_GENERATED_CPP}")

target_include_directories(pulp-native-ui-phase-f-preview PRIVATE
    "${PULP_PHASE_F_PREVIEW_GENERATED_DIR}")

target_link_libraries(pulp-native-ui-phase-f-preview PRIVATE
    pulp::view-core)

target_compile_features(pulp-native-ui-phase-f-preview PRIVATE cxx_std_20)

set_target_properties(pulp-native-ui-phase-f-preview PROPERTIES
    OUTPUT_NAME "PulpNativeUiPhaseFPreview"
    MACOSX_BUNDLE_BUNDLE_NAME "Pulp Native UI Phase F Preview"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.pulp.native-ui-phase-f-preview"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0")

set(PULP_CHAINER_NATIVE_UI_FORMATS VST3 CLAP)
if(APPLE)
    list(APPEND PULP_CHAINER_NATIVE_UI_FORMATS AUv3)
endif()

pulp_add_plugin(PulpChainerNativeUi
    FORMATS         ${PULP_CHAINER_NATIVE_UI_FORMATS}
    SOURCES         chainer_native_ui_processor.cpp
                    "${PULP_PHASE_F_PREVIEW_GENERATED_CPP}"
    PLUGIN_NAME     "PulpChainerNativeUi"
    BUNDLE_ID       "com.pulp.chainer-native-ui"
    MANUFACTURER    "Pulp"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PNui"
    MANUFACTURER_CODE "Pulp"
    DESIGN_WIDTH    900
    DESIGN_HEIGHT   520
    DESIGN_MIN_WIDTH 600
    DESIGN_MIN_HEIGHT 347
    DESIGN_MAX_WIDTH 1800
    DESIGN_MAX_HEIGHT 1040
)

target_include_directories(PulpChainerNativeUi_Core PUBLIC
    "${PULP_PHASE_F_PREVIEW_GENERATED_DIR}")

target_compile_features(PulpChainerNativeUi_Core PUBLIC cxx_std_20)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpChainerNativeUi
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpChainerNativeUi.clap/Contents/MacOS/PulpChainerNativeUi'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpChainerNativeUi PROPERTIES
        LABELS "validation;clap;native-ui"
        TIMEOUT 10)
endif()

if(PULP_HAS_VST3)
    _pulp_find_pluginval(PLUGINVAL)
    if(PLUGINVAL)
        add_test(
            NAME pluginval-PulpChainerNativeUi-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpChainerNativeUi.vst3 --strictness-level 5 --timeout-ms 10000
        )
        set_tests_properties(pluginval-PulpChainerNativeUi-VST3 PROPERTIES
            LABELS "validation;vst3;native-ui"
            TIMEOUT 30
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()
endif()
