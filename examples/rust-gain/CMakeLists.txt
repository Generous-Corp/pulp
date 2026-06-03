# PulpRustGain — a Rust DSP core packaged as a real CLAP plugin.
#
# The native-component capstone: a Rust stereo-gain core (rust-core/) exporting
# the Processor-level C ABI is owned by a C++ NativeCoreProcessor adapter and
# built into a loadable .clap by pulp_add_plugin. Opt-in — needs a Rust
# toolchain, so it is gated behind PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS and
# only builds where the Cargo staticlib lane works (not Windows here).

include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpCargoStaticlib.cmake)

pulp_add_cargo_staticlib(
    NAME pulp-rust-gain-core
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/rust-core/Cargo.toml
    LIB_NAME pulp_rust_gain_core
    PROFILE dev)

pulp_add_plugin(PulpRustGain
    FORMATS         CLAP
    SOURCES         rust_gain.cpp
    PLUGIN_NAME     "PulpRustGain"
    BUNDLE_ID       "com.pulp.rustgain"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PRGn"
    MANUFACTURER_CODE "Pulp")

# Link the Rust staticlib into the shared _Core so every format module inherits
# it; the direct pulp_native_core_entry_v1() reference in rust_gain.cpp pulls
# the archive in.
target_link_libraries(PulpRustGain_Core PUBLIC pulp-rust-gain-core)

# CLAP-dlopen validation: prove the packaged plugin loads (no audio rendered).
if(PULP_HAS_CLAP AND APPLE AND TARGET PulpRustGain_CLAP)
    add_dependencies(PulpRustGain_CLAP pulp-rust-gain-core)
    add_test(
        NAME clap-dlopen-PulpRustGain
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpRustGain.clap/Contents/MacOS/PulpRustGain'); print('OK')\"")
    set_tests_properties(clap-dlopen-PulpRustGain PROPERTIES
        LABELS "validation;clap;native-components" TIMEOUT 15)
endif()
