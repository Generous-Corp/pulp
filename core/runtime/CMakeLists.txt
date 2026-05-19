add_library(pulp-runtime)
add_library(pulp::runtime ALIAS pulp-runtime)

target_include_directories(pulp-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-runtime PRIVATE
    src/runtime.cpp
    src/identity.cpp
    src/scoped_no_alloc.cpp
    src/simd.cpp
    src/memory_mapped_file.cpp
    src/temporary_file.cpp
    src/dynamic_library.cpp
    src/child_process.cpp
    src/inter_process_lock.cpp
    src/base64.cpp
    src/high_resolution_timer.cpp
    src/system_info.cpp
    src/text_diff.cpp
    src/xml.cpp
    src/zip.cpp
    src/http.cpp
    src/socket.cpp
    src/named_pipe.cpp
    src/stream.cpp
    src/async_stream.cpp
    src/network_stream.cpp
    src/memory_message_channel.cpp
    src/websocket_channel.cpp
    src/json_rpc.cpp
    src/ip_address.cpp
    src/i18n.cpp
    src/crypto.cpp
    src/analytics.cpp
    src/big_integer.cpp
    src/license.cpp
    src/primes.cpp
    src/expression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml/pugixml.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz/miniz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz/miniz_tdef.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz/miniz_tinfl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz/miniz_zip.c
)

# Highway's foreach_target.h uses #include HWY_TARGET_INCLUDE to re-include
# the source file for each SIMD target. The source dir must be on the include path.
target_include_directories(pulp-runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Vendored dependencies (header-only or single-file)
target_include_directories(pulp-runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/cpp-httplib
)

target_link_libraries(pulp-runtime PUBLIC pulp::platform)
target_link_libraries(pulp-runtime PRIVATE hwy)

# Mbed TLS for cryptographic operations
if(TARGET mbedcrypto)
    target_link_libraries(pulp-runtime PRIVATE mbedcrypto mbedx509 mbedtls)
    target_include_directories(pulp-runtime PRIVATE
        ${mbedtls_SOURCE_DIR}/include
    )
endif()

# CHOC headers (used by SpscQueue wrapper)
target_include_directories(pulp-runtime PUBLIC
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
