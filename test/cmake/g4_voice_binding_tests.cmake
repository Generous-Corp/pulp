# Installed public SDK consumer for the bounded G-4 voice-bank persistence contract.
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME cmake-g4-voice-bank-sdk-consumer
        COMMAND ${CMAKE_COMMAND}
            "-DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DPULP_PARENT_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DPULP_PARENT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DPULP_PARENT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            "-DPULP_PARENT_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
            -P "${CMAKE_CURRENT_LIST_DIR}/test_g4_voice_binding_sdk_consumer.cmake")
    set_tests_properties(cmake-g4-voice-bank-sdk-consumer PROPERTIES
        LABELS "cmake;sdk;host;needs-build" TIMEOUT 600)
endif()
