# The backend contract has no external RmlUi/Lua dependency.  It is only
# materialized when explicitly enabled, so default builds cannot acquire it.
if(PULP_ENABLE_RMLUI)
    pulp_add_test_suite(pulp-test-rmlui-backend
        SOURCES
            test_rmlui_backend.cpp
            test_rmlui_core_smoke.cpp
        LIBRARIES pulp::view-rmlui)
    if(PULP_ENABLE_RMLUI_FONTS)
        target_compile_definitions(pulp-test-rmlui-backend PRIVATE
            PULP_RMLUI_TEST_FONT_PATH="${PULP_ROOT_DIR}/external/fonts/Inter-Regular.ttf")
    endif()
else()
    add_test(NAME pulp.rmlui-default-off
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_LIST_DIR}/assert_rmlui_default_off.cmake)
    set_tests_properties(pulp.rmlui-default-off PROPERTIES LABELS "view;rmlui;isolation")
endif()
