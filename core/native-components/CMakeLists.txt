# core/native-components — the language-neutral native-component core ABI.
#
# Header-only contract (native_core.h / native_core.hpp) plus a tiny production
# source that supplies the weak default RT-safety trap. This target has NO Rust
# dependency and builds on every platform, including iOS — it is just the C ABI
# surface a C++ Processor adapter or a native (Rust/C/Zig) core compiles against.
#
# The opt-in Rust staticlib lane (cargo build of a core + RT-checking allocator)
# lives in the test tree behind PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS, driven by
# tools/cmake/PulpCargoStaticlib.cmake — NOT here, so default builds never need
# cargo/rustc.
add_library(pulp-native-components STATIC)
add_library(pulp::native-components ALIAS pulp-native-components)

target_include_directories(pulp-native-components
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-native-components PRIVATE
    src/native_core.cpp
)

target_compile_features(pulp-native-components PUBLIC cxx_std_20)
