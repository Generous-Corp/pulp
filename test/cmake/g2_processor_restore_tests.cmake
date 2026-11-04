if(TARGET pulp-host)
    add_executable(pulp-test-processor-restore
        "${CMAKE_SOURCE_DIR}/examples/g2-processor-restore-consumer/main.cpp")
    target_link_libraries(pulp-test-processor-restore PRIVATE pulp::host)
    target_compile_features(pulp-test-processor-restore PRIVATE cxx_std_20)
    add_test(NAME processor-restore-contract COMMAND pulp-test-processor-restore)
    set_tests_properties(processor-restore-contract PROPERTIES LABELS "host;state" TIMEOUT 30)
    if(UNIX AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME cmake-g2-processor-restore-sdk-consumer
            COMMAND ${CMAKE_COMMAND}
                "-DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DPULP_PARENT_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
                "-DPULP_PARENT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DPULP_PARENT_SANITIZER=${PULP_SANITIZER}"
                "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
                "-DPULP_PARENT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                "-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=${_sdk_consumer_instrumentation_compile_flags}"
                "-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=${_sdk_consumer_instrumentation_link_flags}"
                "-DPULP_PARENT_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_g2_processor_restore_sdk_consumer.cmake")
        set_tests_properties(cmake-g2-processor-restore-sdk-consumer PROPERTIES
            LABELS "cmake;sdk;host;needs-build" TIMEOUT 600)
    endif()
endif()
