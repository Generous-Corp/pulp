# Installed-SDK G3 consumers: each stages the producer's install and builds an
# examples/ project through find_package(Pulp COMPONENTS host), proving the
# public headers and exported targets rather than the in-tree build.
#   g3-resolved-identity-consumer: resolved identity receipts.
#   g3-d4-prepared-export-consumer: prepared serial I/O + Gain export.
if(TARGET pulp-host AND UNIX AND NOT CMAKE_CROSSCOMPILING)
    foreach(_g3_consumer g3-resolved-identity-consumer g3-d4-prepared-export-consumer)
        add_test(NAME cmake-${_g3_consumer}
            COMMAND ${CMAKE_COMMAND}
                "-DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DPULP_CONSUMER_EXAMPLE=${_g3_consumer}"
                "-DPULP_PARENT_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
                "-DPULP_PARENT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DPULP_PARENT_SANITIZER=${PULP_SANITIZER}"
                "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
                "-DPULP_PARENT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                "-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=${_sdk_consumer_instrumentation_compile_flags}"
                "-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=${_sdk_consumer_instrumentation_link_flags}"
                "-DPULP_PARENT_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
                -P "${CMAKE_CURRENT_LIST_DIR}/test_g3_installed_consumer.cmake")
        set_tests_properties(cmake-${_g3_consumer} PROPERTIES
            LABELS "cmake;sdk;host;graph;needs-build" TIMEOUT 900)
    endforeach()
endif()
