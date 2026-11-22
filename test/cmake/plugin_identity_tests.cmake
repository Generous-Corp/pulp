if(Python3_Interpreter_FOUND)
    add_test(NAME pulp.plugin-identity-contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/cmake/test_pulp_plugin_identity.py")
    set_tests_properties(pulp.plugin-identity-contract PROPERTIES LABELS "pr")
endif()
