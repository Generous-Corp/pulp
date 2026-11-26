find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-components
        ${CMAKE_CURRENT_SOURCE_DIR}/arranger_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/panic_button_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lasso_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/model_manager_view_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/preferences_panel_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/catalogue_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector_window_test.cpp)
    # The timeline fixtures are shared with the core timeline tests.
    target_include_directories(pulp-test-pulp-vellum-components PRIVATE
        ${PULP_ROOT_DIR}/test)
    target_link_libraries(pulp-test-pulp-vellum-components PRIVATE
        pulp::vellum-components Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-components PROPERTIES LABELS pulp-vellum-components)
endif()
