# pulp::simd — the portable block-kernel library.
#
# A small compiled target so the header-only DSP layer (pulp::signal) can call
# vectorized kernels without linking pulp-runtime, whose archive carries the
# HTTP/TLS stack. Its only link item is Highway, which the clean-consumer
# contract accepts as a DSP dependency.
add_library(pulp-simd STATIC
    src/highway.cpp
)
add_library(pulp::simd ALIAS pulp-simd)

target_compile_features(pulp-simd PUBLIC cxx_std_20)

target_include_directories(pulp-simd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Highway's foreach_target re-includes the kernel TU by this path.
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pulp-simd PRIVATE hwy)

# Highway's HWY_ABORT uses the `, ##__VA_ARGS__` extension, which -Wpedantic
# reports once per dispatched target. The code is third-party; silence only
# that diagnostic, only here.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pulp-simd PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
endif()
