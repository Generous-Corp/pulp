#pragma once

// WebCLAP (WCLAP) adapter for Pulp
// Exports a Pulp plugin as a WCLAP — a CLAP plugin compiled to WebAssembly.
//
// WebCLAP spec (https://github.com/WebCLAP):
//   A WCLAP is a .wasm module that exports clap_entry and conforms to the
//   CLAP plugin API. It can run in:
//     - Native DAWs via wclap-bridge (Wasmtime runtime)
//     - Browsers via wclap-host-js (AudioWorklet + WebAssembly)
//
// Requirements for a valid WCLAP:
//   1. Export `clap_entry` (the standard CLAP entry point)
//   2. Support shared or exported memory
//   3. Export a growable function table
//   4. Provide malloc() or cabi_realloc() for host memory allocation
//   5. Build with WASI SDK (wasi_snapshot_preview1)
//
// This adapter provides:
//   - WCLAP entry point generation via PULP_WCLAP_PLUGIN() macro
//   - Memory allocation exports for the WCLAP host sandbox
//   - WASI-compatible build configuration
//
// Build:
//   Uses wasi-sdk (not Emscripten) to target wasm32-wasi.
//   The output is a .wasm file that exports clap_entry.

#include <pulp/format/processor.hpp>
#include <cstddef>

namespace pulp::format::wclap {

// Memory allocator export required by WebCLAP hosts.
// The host calls this to allocate memory inside the WASM sandbox
// for passing strings, events, and other data to the plugin.
extern "C" void* wclap_malloc(size_t size);
extern "C" void wclap_free(void* ptr);

// Compatibility alias — WebCLAP hosts may call either name
extern "C" void* cabi_realloc(void* ptr, size_t old_size,
                               size_t align, size_t new_size);

} // namespace pulp::format::wclap

// Generate a WCLAP entry point.
// This reuses the existing CLAP adapter and entry point,
// adding the memory allocation exports required by WebCLAP.
//
// Usage (in one .cpp file per WCLAP plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/web/wclap_adapter.hpp>
//   #include <pulp/format/clap_entry.hpp>
//
//   PULP_WCLAP_PLUGIN("com.mycompany.myplugin", "My Plugin",
//                      "My Company", "1.0.0",
//                      my_namespace::create_my_processor)
//
// Build with wasi-sdk:
//   /opt/wasi-sdk/bin/clang++ --target=wasm32-wasi \
//     -mexported-symbols=clap_entry,malloc,free \
//     -o MyPlugin.wasm wclap_entry.cpp ...

// id/name/vendor/version come from the Processor's descriptor() (see
// PULP_CLAP_PLUGIN); the WCLAP macro only needs the factory plus the WebCLAP
// memory-allocation exports the host sandbox requires.
#define PULP_WCLAP_PLUGIN(factory_fn) \
    PULP_CLAP_PLUGIN(factory_fn)      \
                                                                   \
    /* WebCLAP memory allocation exports */                        \
    extern "C" {                                                   \
        __attribute__((export_name("malloc")))                     \
        void* wclap_malloc(size_t size) {                          \
            return ::malloc(size);                                 \
        }                                                          \
        __attribute__((export_name("free")))                       \
        void wclap_free(void* ptr) {                               \
            ::free(ptr);                                           \
        }                                                          \
        __attribute__((export_name("cabi_realloc")))               \
        void* cabi_realloc(void* ptr, size_t old_size,             \
                           size_t, size_t new_size) {              \
            (void)old_size;                                        \
            if (new_size == 0) { ::free(ptr); return nullptr; }    \
            return ::realloc(ptr, new_size);                       \
        }                                                          \
    }
