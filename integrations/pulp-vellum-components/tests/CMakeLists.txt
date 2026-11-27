find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-components
        ${CMAKE_CURRENT_SOURCE_DIR}/arranger_session_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/arranger_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/panic_button_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lasso_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/model_manager_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/preferences_panel_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ab_compare_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/property_panel_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/catalogue_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/preset_program_editor_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu_health_content_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_window_test.cpp)
    # The timeline fixtures are shared with the core timeline tests.
    target_include_directories(pulp-test-pulp-vellum-components PRIVATE
        ${PULP_ROOT_DIR}/test)
    target_link_libraries(pulp-test-pulp-vellum-components PRIVATE
        pulp::vellum-components Catch2::Catch2WithMain Threads::Threads)
    target_compile_definitions(pulp-test-pulp-vellum-components PRIVATE
        PULP_VELLUM_COMPONENTS_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../include")
    # With Pulp's own view layer present the same fixtures also run through it,
    # so the two floors are compared decision for decision.
    if(TARGET pulp-view-core)
        target_link_libraries(pulp-test-pulp-vellum-components PRIVATE pulp::view-core)
        target_compile_definitions(pulp-test-pulp-vellum-components PRIVATE
            PULP_VELLUM_COMPONENTS_HAS_PULP_VIEW=1)
    endif()
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-components PROPERTIES LABELS pulp-vellum-components)
endif()
