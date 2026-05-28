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
