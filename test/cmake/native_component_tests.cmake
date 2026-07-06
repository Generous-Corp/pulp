# Native component ABI, node-pack, and optional Rust lane tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Child process tests
if(WIN32)
    pulp_add_test_suite(pulp-test-child-process
        LIBRARIES pulp::platform
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-child-process LIBRARIES pulp::platform)
endif()

# Progress parser tests
pulp_add_test_suite(pulp-test-progress-parser LIBRARIES pulp::platform)

# Stream tests (unified I/O interface)
pulp_add_test_suite(pulp-test-stream LIBRARIES pulp::runtime)

# Generic model registry/store.
pulp_add_test_suite(pulp-test-model-store LIBRARIES pulp::runtime)

# Streaming model downloader; uses a local httplib server fixture.
# Link pulp-cpp-httplib so this TU compiles httplib.h with the SAME
# CPPHTTPLIB_MBEDTLS_SUPPORT macro as pulp-runtime (else httplib's inline
# functions ODR-fold across the two layouts → corrupt mutex → EINVAL).
pulp_add_test_suite(pulp-test-model-download LIBRARIES pulp::runtime pulp-cpp-httplib)

# ModelManager view; needs the view stack (pulp::view) for render_to_png.
add_executable(pulp-test-model-manager-view test_model_manager_view.cpp)
target_link_libraries(pulp-test-model-manager-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-model-manager-view)

# Native-component core ABI contract tests. Always built (no Rust toolchain
# needed) — one case per forward-compatibility decision (1-12) so the C ABI
# shape can't silently drift. The Rust round-trip + RT-safety self-test is the
# separate opt-in lane below (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS).
pulp_add_test_suite(pulp-test-native-core-ffi
    SOURCES test_native_core_ffi.cpp
    LIBRARIES pulp::native-components)

# NativeCoreProcessor seam + RT-safety harnesses; no Rust required.
pulp_add_test_suite(pulp-test-native-core-processor
    SOURCES test_native_core_processor.cpp test_rt_safety.cpp test_signal_graph_rt_safety.cpp $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp> $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::format ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)

# Public node ABI (pulp_node_v1) conformance — always built, no Rust. Pins the
# ABI shape, version negotiation, and a C node's full lifecycle.
pulp_add_test_suite(pulp-test-pulp-node-v1
    SOURCES test_pulp_node_v1.cpp
    LIBRARIES pulp::native-components pulp::runtime)

# Signed node-pack loader. Build a real loadable node MODULE and a
# test that signs its manifest (Ed25519) then verifies trusted load + every
# rejection path (untrusted/bad-sig/hash-mismatch/abi-mismatch/missing). Desktop
# only: pulp::host (and dlopen) is compiled out on iOS.
if(TARGET pulp::host)
    add_library(pulp-test-node-pack-module MODULE
        fixtures/native-components/node_pack_test_module.cpp)
    target_link_libraries(pulp-test-node-pack-module PRIVATE pulp::native-components)
    # Only the explicitly-exported pulp_node_v1_entry should be visible.
    set_target_properties(pulp-test-node-pack-module PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    pulp_add_test_suite(pulp-test-node-pack
        SOURCES test_node_pack.cpp
        LIBRARIES pulp::host pulp::native-components pulp::runtime)
    add_dependencies(pulp-test-node-pack pulp-test-node-pack-module)
    target_compile_definitions(pulp-test-node-pack PRIVATE
        PULP_NODE_PACK_MODULE_PATH="$<TARGET_FILE:pulp-test-node-pack-module>")
endif()

# Opt-in Rust lane: build a Cargo staticlib reference core (with an RT-checking
# global allocator) and link the FFI round-trip + RT-safety death tests. OFF by
# default, so default builds need no Rust toolchain. The death tests fork() a
# child that deliberately allocates inside a no-alloc scope and assert it is
# trapped — never run in the Catch2 parent (Unix-only; the lane is desktop).
if(PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS AND NOT WIN32)
    include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpCargoStaticlib.cmake)
    pulp_add_cargo_staticlib(
        NAME pulp-noop-rust-core
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native-components/noop-rust-core/Cargo.toml
        LIB_NAME pulp_noop_rust_core
        FEATURES pulp_rt_check_allocator
        PROFILE dev)
    pulp_add_test_suite(pulp-test-rust-dsp-ffi
        SOURCES test_rust_dsp_ffi.cpp native_components/rt_intercept_test_support.cpp
        LIBRARIES pulp::native-components pulp::runtime pulp-noop-rust-core)

    # Real Rust gain DSP core driven through the C++
    # NativeCoreProcessor adapter — proves native audio end-to-end.
    pulp_add_cargo_staticlib(
        NAME pulp-gain-rust-core
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native-components/gain-rust-core/Cargo.toml
        LIB_NAME pulp_gain_rust_core
        PROFILE dev)
    pulp_add_test_suite(pulp-test-rust-dsp-processor
        SOURCES test_rust_dsp_processor.cpp
        LIBRARIES pulp::format pulp-gain-rust-core)

    # Rust node implementing the public pulp_node_v1 ABI, proving a C
    # node and a Rust node load through the same contract.
    pulp_add_cargo_staticlib(
        NAME pulp-node-rust
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native-components/node-rust/Cargo.toml
        LIB_NAME pulp_node_rust
        PROFILE dev)
    pulp_add_test_suite(pulp-test-pulp-node-v1-rust
        SOURCES test_pulp_node_v1_rust.cpp
        LIBRARIES pulp::native-components pulp-node-rust)
endif()
