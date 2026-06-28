# PulpWclap.cmake — WebCLAP (WCLAP) build support
#
# Builds Pulp plugins as WCLAP modules (.wasm) using wasi-sdk.
# WCLAPs are CLAP plugins compiled to WebAssembly that can run in:
#   - Native DAWs via wclap-bridge (Wasmtime)
#   - Browsers via wclap-host-js (AudioWorklet)
#
# Prerequisites:
#   - wasi-sdk installed (https://github.com/WebAssembly/wasi-sdk)
#   - Set WASI_SDK_PREFIX to the installation path
#
# Usage:
#   cmake -S . -B build-wclap \
#     -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake
#
# Use this helper from your CMakeLists.txt before configuring the project:
#   pulp_add_wclap(MyPlugin
#       PLUGIN_ID "com.mycompany.myplugin"
#       PLUGIN_NAME "My Plugin"
#       VENDOR "My Company"
#       VERSION "1.0.0"
#       FACTORY my_namespace::create_my_processor
#   )

# Detect wasi-sdk
if(DEFINED ENV{WASI_SDK_PREFIX})
    set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PREFIX}")
elseif(EXISTS "/opt/wasi-sdk")
    set(WASI_SDK_PREFIX "/opt/wasi-sdk")
endif()

# Helper: build a WCLAP target
# Creates a .wasm file that exports clap_entry + memory allocators
function(pulp_add_wclap target)
    cmake_parse_arguments(WCLAP
        ""
        "PLUGIN_ID;PLUGIN_NAME;VENDOR;VERSION;FACTORY"
        "SOURCES;LINK_LIBS"
        ${ARGN}
    )

    if(NOT WCLAP_PLUGIN_ID OR NOT WCLAP_FACTORY)
        message(FATAL_ERROR "pulp_add_wclap: PLUGIN_ID and FACTORY are required")
    endif()

    # Generate the WCLAP entry point source
    set(WCLAP_ENTRY_SRC "${CMAKE_CURRENT_BINARY_DIR}/${target}_wclap_entry.cpp")
    file(WRITE "${WCLAP_ENTRY_SRC}" "
// Auto-generated WCLAP entry point for ${WCLAP_PLUGIN_NAME}
#include <pulp/format/web/wclap_adapter.hpp>

// Forward-declare the factory
namespace { auto& _factory = ${WCLAP_FACTORY}; }

PULP_WCLAP_PLUGIN(
    \"${WCLAP_PLUGIN_ID}\",
    \"${WCLAP_PLUGIN_NAME}\",
    \"${WCLAP_VENDOR}\",
    \"${WCLAP_VERSION}\",
    ${WCLAP_FACTORY}
)
")

    add_executable(${target}_WCLAP
        ${WCLAP_ENTRY_SRC}
        ${WCLAP_SOURCES}
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )

    target_link_libraries(${target}_WCLAP PRIVATE
        pulp::format
        ${WCLAP_LINK_LIBS}
    )

    # WCLAP-specific compile/link flags
    target_compile_definitions(${target}_WCLAP PRIVATE
        PULP_WCLAP=1
        PULP_CLAP_GUI=0
    )

    # Export symbols required by WebCLAP hosts
    set_target_properties(${target}_WCLAP PROPERTIES
        OUTPUT_NAME "${WCLAP_PLUGIN_NAME}"
        SUFFIX ".wasm"
    )

    if(WASI_SDK_PREFIX)
        target_link_options(${target}_WCLAP PRIVATE
            -Wl,--export=clap_entry
            -Wl,--export=malloc
            -Wl,--export=free
            -Wl,--export=cabi_realloc
            -Wl,--export-table
            -Wl,--growable-table
        )
    endif()
endfunction()
