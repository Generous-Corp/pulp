# Minimal CV vertical slice. Capture/inference stays application-owned.
if(PULP_BUILD_TESTS)
    add_executable(pulp-cv-control-test test_cv_control.cpp)
    target_link_libraries(pulp-cv-control-test PRIVATE pulp::format pulp::osc Catch2::Catch2WithMain)
    target_include_directories(pulp-cv-control-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-cv-control-test)
endif()

add_executable(pulp-cv-control-standalone main.cpp)
target_link_libraries(pulp-cv-control-standalone PRIVATE pulp::format pulp::standalone pulp::osc)
target_include_directories(pulp-cv-control-standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pulp-cv-control-standalone PRIVATE cxx_std_20)

set(CV_FACE_TONE_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND CV_FACE_TONE_FORMATS AU)
endif()

pulp_add_plugin(CvFaceTone
    FORMATS ${CV_FACE_TONE_FORMATS}
    PLUGIN_NAME "CV Face Tone"
    BUNDLE_ID "com.pulp.cv-face-tone"
    MANUFACTURER "Pulp"
    VERSION "0.1.0"
    CATEGORY Instrument
    PLUGIN_CODE "CvFt"
    MANUFACTURER_CODE "Pulp")
target_include_directories(CvFaceTone_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(CvFaceTone_Core INTERFACE pulp::osc)
