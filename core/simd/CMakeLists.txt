# pulp::simd — the portable block-kernel library.
#
# A small compiled target so the header-only DSP layer (pulp::signal) can call
# vectorized kernels without linking pulp-runtime, whose archive carries the
# HTTP/TLS stack. Its link items are Highway, which the clean-consumer
# contract accepts as a DSP dependency, and Accelerate on Apple, which
# pulp::signal already links for its FFT.
#
# Every backend that can build on the platform is compiled (scalar always,
# Highway when the target supports it, Accelerate on Apple) so the parity
# suite and the throughput benchmark can compare them in one binary.
# ARM64EC targets deliberately omit Highway because MSVC exposes both ARM64
# and x64 architecture macros there and Highway rejects that combination.
# PULP_SIMD_BACKEND picks which one the unqualified pulp::simd:: names alias;
# it is resolved at configure time, so a call carries no run-time backend
# branch.
set(PULP_SIMD_BACKEND "auto" CACHE STRING
    "Backend behind pulp::simd: auto (Accelerate on Apple, Highway elsewhere, scalar when unavailable), accelerate, highway, scalar")
set_property(CACHE PULP_SIMD_BACKEND PROPERTY STRINGS auto accelerate highway scalar)

set(_pulp_simd_backend "${PULP_SIMD_BACKEND}")
if(_pulp_simd_backend STREQUAL "auto")
    if(APPLE)
        set(_pulp_simd_backend accelerate)
    elseif(TARGET hwy)
        set(_pulp_simd_backend highway)
    else()
        set(_pulp_simd_backend scalar)
    endif()
endif()
if(NOT _pulp_simd_backend MATCHES "^(accelerate|highway|scalar)$")
    message(FATAL_ERROR "PULP_SIMD_BACKEND='${PULP_SIMD_BACKEND}' is not one of "
        "auto, accelerate, highway, scalar.")
endif()
if(_pulp_simd_backend STREQUAL "accelerate" AND NOT APPLE)
    message(FATAL_ERROR "PULP_SIMD_BACKEND=accelerate requires an Apple platform.")
endif()
if(_pulp_simd_backend STREQUAL "highway" AND NOT TARGET hwy)
    message(FATAL_ERROR
        "PULP_SIMD_BACKEND=highway requires a Highway target; this target "
        "does not support Highway (ARM64EC uses the scalar backend).")
endif()
string(TOUPPER "${_pulp_simd_backend}" _pulp_simd_backend_upper)
set(PULP_SIMD_RESOLVED_BACKEND "${_pulp_simd_backend}" CACHE INTERNAL
    "Backend behind the unqualified pulp::simd names")
message(STATUS "Pulp SIMD backend: ${_pulp_simd_backend} (PULP_SIMD_BACKEND=${PULP_SIMD_BACKEND})")

set(_pulp_simd_sources src/scalar.cpp)
if(TARGET hwy)
    list(APPEND _pulp_simd_sources src/highway.cpp)
endif()
add_library(pulp-simd STATIC ${_pulp_simd_sources})
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

target_compile_definitions(pulp-simd PUBLIC
    PULP_SIMD_DEFAULT_BACKEND_${_pulp_simd_backend_upper}=1)

if(TARGET hwy)
    target_compile_definitions(pulp-simd PUBLIC PULP_SIMD_HAS_HIGHWAY=1)
    target_link_libraries(pulp-simd PRIVATE hwy)
endif()

if(APPLE)
    target_sources(pulp-simd PRIVATE src/accelerate.cpp)
    target_compile_definitions(pulp-simd PUBLIC PULP_SIMD_HAS_ACCELERATE=1)
    target_link_libraries(pulp-simd PUBLIC "-framework Accelerate")
endif()

unset(_pulp_simd_backend)
unset(_pulp_simd_backend_upper)
unset(_pulp_simd_sources)

# Highway's HWY_ABORT uses the `, ##__VA_ARGS__` extension, which -Wpedantic
# reports once per dispatched target. The code is third-party; silence only
# that diagnostic, only here.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pulp-simd PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
endif()
