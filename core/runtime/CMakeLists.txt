add_library(pulp-runtime)
add_library(pulp::runtime ALIAS pulp-runtime)

# ── pulp/runtime/build_info.hpp (Tier A Slice 7) ───────────────────────────
# Generated at configure time so the running binary can stamp itself with
# the CMake build type, an ISO 8601 configure timestamp, and the short git
# SHA (plus a dirty-tree flag). Per sudara "Big List of JUCE Tips" #9 —
# the cheapest "which build is this?" tool is a label in your editor or
# log header. Header lands under <build>/generated/pulp/runtime/build_info.hpp
# and is exposed on the PUBLIC include path of pulp-runtime so any
# consumer can `#include <pulp/runtime/build_info.hpp>`.
set(_pulp_build_info_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_pulp_build_info_path
    "${_pulp_build_info_dir}/pulp/runtime/build_info.hpp")

string(TIMESTAMP PULP_BUILD_ISO8601 "%Y-%m-%dT%H:%M:%SZ" UTC)
set(PULP_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
set(PULP_VERSION "${PROJECT_VERSION}")

# Best-effort git short SHA + dirty-tree probe. Empty when the source
# tree isn't a git checkout (distribution tarball) — the generated
# header documents that case.
set(PULP_GIT_SHA "")
set(PULP_GIT_DIRTY "false")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE PULP_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" status --porcelain
        OUTPUT_VARIABLE _pulp_git_porcelain
        ERROR_QUIET)
    if(NOT "${_pulp_git_porcelain}" STREQUAL "")
        set(PULP_GIT_DIRTY "true")
    endif()
endif()

# Compose a single ready-to-display label so the running app can just
# print kStampLabel without re-formatting.
#
# Multi-config generators (Xcode, Visual Studio) leave CMAKE_BUILD_TYPE
# empty at configure time — the config is chosen per build invocation.
# Stamp label composition must still produce a useful value in that
# case: it falls back to "config@build" with the SHA + ISO timestamp
# still appearing, so the inspector / About dialog never renders an
# information-free label. Codex P2 on PR #2335.
set(PULP_BUILD_STAMP "${PULP_VERSION}")
set(_pulp_build_label "${PULP_BUILD_TYPE}")
if(NOT _pulp_build_label)
    set(_pulp_build_label "config@build")
endif()
string(APPEND PULP_BUILD_STAMP " (${_pulp_build_label}")
if(PULP_GIT_SHA)
    string(APPEND PULP_BUILD_STAMP ", ${PULP_GIT_SHA}")
    if(PULP_GIT_DIRTY STREQUAL "true")
        string(APPEND PULP_BUILD_STAMP "+dirty")
    endif()
endif()
string(APPEND PULP_BUILD_STAMP ", ${PULP_BUILD_ISO8601})")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pulp/runtime/build_info.hpp.in"
    "${_pulp_build_info_path}"
    @ONLY)

# Install the generated header so consumers using `find_package(Pulp)`
# after `cmake --install` can `#include <pulp/runtime/build_info.hpp>`
# the same way they do during a source-tree build. PulpInstallRules.cmake
# only globs core/*/include for the canonical headers; the build-time-
# generated header has to be installed explicitly. Codex P2 on PR #2335.
install(
    FILES "${_pulp_build_info_path}"
    DESTINATION include/pulp/runtime)

target_include_directories(pulp-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${_pulp_build_info_dir}>
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
    src/mac_address.cpp
    src/url.cpp
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
    # TweetNaCl (public domain) — Ed25519 sign/verify per RFC 8032.
    # `randombytes()` is supplied by core/runtime/src/crypto.cpp,
    # wired to the runtime's mbedTLS CTR-DRBG instance.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/tweetnacl/tweetnacl.c
)

# Highway's foreach_target.h uses #include HWY_TARGET_INCLUDE to re-include
# the source file for each SIMD target. The source dir must be on the include path.
target_include_directories(pulp-runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Vendored dependencies (header-only or single-file)
target_include_directories(pulp-runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external
)

set(_pulp_ubsan_enabled OFF)
if(PULP_SANITIZER STREQUAL "undefined"
        OR CMAKE_CXX_FLAGS MATCHES "(^| )-fsanitize=[^ ]*undefined"
        OR CMAKE_C_FLAGS MATCHES "(^| )-fsanitize=[^ ]*undefined")
    set(_pulp_ubsan_enabled ON)
endif()

if(_pulp_ubsan_enabled AND CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    # TweetNaCl intentionally uses signed-bit arithmetic patterns that trip
    # UBSan. Keep sanitizer coverage on Pulp's Ed25519 wrappers and tests
    # while compiling only the vendored C implementation without UBSan.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/tweetnacl/tweetnacl.c
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined")
endif()
unset(_pulp_ubsan_enabled)

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
