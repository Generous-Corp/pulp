# Installed SDK consumer for the optional RmlUi bridge. This verifies both
# Pulp's exported bridge and RmlUi's direct package entry point.
if(PULP_ENABLE_RMLUI)
    add_test(NAME cmake-rmlui-sdk-consumer
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_PARENT_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -P ${CMAKE_CURRENT_LIST_DIR}/test_rmlui_sdk_consumer.cmake)
    set_tests_properties(cmake-rmlui-sdk-consumer PROPERTIES
        LABELS "cmake;sdk;rmlui"
        TIMEOUT 180)
endif()
