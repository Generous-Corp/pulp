find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-components
        ${CMAKE_CURRENT_SOURCE_DIR}/panic_button_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lasso_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/model_manager_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/preferences_panel_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/catalogue_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/overlay_geometry_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_measure_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_loupe_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_field_edit_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/text_edit_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_text_edit_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_pass_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_atlas_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_overlay_knob_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/knob_panel_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_window_test.cpp)
    target_link_libraries(pulp-test-pulp-vellum-components PRIVATE
        pulp::vellum-components Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-components PROPERTIES LABELS pulp-vellum-components)
endif()
