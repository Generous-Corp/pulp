# PulpWasm.cmake — Emscripten/WASM build configuration for Pulp
#
# Usage:
#   emcmake cmake -S . -B build-wasm -DPULP_WASM=ON
#   cmake --build build-wasm
#
# Prerequisites:
#   - Emscripten SDK installed (emsdk activate latest)
#   - emcmake / emcc on PATH
#
# This module:
#   - Detects Emscripten and sets PULP_WASM=ON
#   - Configures appropriate compile/link flags
#   - Adds Web Audio API and Web MIDI API integration
#   - Enables WebGPU when available in the browser
#   - Creates .html/.js/.wasm output for each standalone target

# Detect Emscripten
if(EMSCRIPTEN OR PULP_WASM)
    set(PULP_WASM ON CACHE BOOL "Building for WebAssembly" FORCE)
    message(STATUS "Pulp: WASM build enabled (Emscripten)")

    # Global compile options for WASM
    add_compile_options(
        -sUSE_PTHREADS=0          # Single-threaded for now (SharedArrayBuffer requires COOP/COEP)
        -fno-exceptions            # Smaller binary, audio code doesn't use exceptions
    )

    # Default link flags for all WASM executables
    set(PULP_WASM_LINK_FLAGS
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
        "-sENVIRONMENT=web"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sSTACK_SIZE=1048576"     # 1MB stack
        "--bind"                   # Embind for JS interop
    )

    # WebGPU support (browser-native, no Dawn needed)
    set(PULP_WASM_WEBGPU_FLAGS
        "-sUSE_WEBGPU=1"
    )

    # Audio worklet flags
    set(PULP_WASM_AUDIO_FLAGS
        "-sAUDIO_WORKLET=0"        # We use our own JS Audio Worklet bridge
    )

    # Helper: create a WASM standalone target
    # Creates <target>.html, <target>.js, <target>.wasm
    function(pulp_add_wasm_app target)
        cmake_parse_arguments(WASM
            "WEBGPU"
            "APP_NAME;SHELL_FILE"
            "SOURCES;LINK_LIBS"
            ${ARGN}
        )

        add_executable(${target} ${WASM_SOURCES})
        target_link_libraries(${target} PRIVATE ${WASM_LINK_LIBS} pulp::format)

        # Base link flags
        set(link_flags ${PULP_WASM_LINK_FLAGS} ${PULP_WASM_AUDIO_FLAGS})

        # WebGPU
        if(WASM_WEBGPU)
            list(APPEND link_flags ${PULP_WASM_WEBGPU_FLAGS})
        endif()

        # Shell file
        if(WASM_SHELL_FILE)
            list(APPEND link_flags "--shell-file" "${WASM_SHELL_FILE}")
        endif()

        string(JOIN " " link_flags_str ${link_flags})
        set_target_properties(${target} PROPERTIES
            SUFFIX ".html"
            LINK_FLAGS "${link_flags_str}"
        )

        if(WASM_APP_NAME)
            set_target_properties(${target} PROPERTIES OUTPUT_NAME "${WASM_APP_NAME}")
        endif()
    endfunction()
else()
    set(PULP_WASM OFF)
endif()
