find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-audio
        ${PULP_ROOT_DIR}/test/test_pulp_vellum_audio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/eq_curve_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/multi_meter_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/register_audio_widgets_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/musical_typing_keyboard_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/musical_typing_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/piano_roll_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/spectrogram_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/step_grid_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/waveform_editor_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/waveform_recorder_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/waveform_view_test.cpp)
    # The timeline fixtures are shared with the core timeline tests.
    target_include_directories(pulp-test-pulp-vellum-audio PRIVATE ${PULP_ROOT_DIR}/test)
    target_link_libraries(pulp-test-pulp-vellum-audio PRIVATE
        pulp::vellum-audio Vellum::MirrorView_script Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-audio PROPERTIES LABELS pulp-vellum-audio)
endif()

if(TARGET pulp::vellum-widget-gallery AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-vellum-widget-gallery test_widget_gallery.cpp)
    target_link_libraries(pulp-test-vellum-widget-gallery PRIVATE
        pulp::vellum-widget-gallery Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(pulp-test-vellum-widget-gallery PROPERTIES LABELS pulp-vellum)
endif()
