add_library(pulp-signal INTERFACE)
add_library(pulp::signal ALIAS pulp-signal)

target_include_directories(pulp-signal
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Public signal headers use music vocabulary, runtime's header-only queues, and
# timebase's canonical persisted beat-division table. Propagate those so
# consumers of pulp::signal do not have to know its header closure.
#
# pulp::foundation, not pulp::runtime: the only runtime headers the DSP reaches
# for are TripleBuffer, SeqLock and Slot, all templates with no object code
# behind them. Naming the full runtime target instead would put its mbedTLS
# link items on the link command of anything that computes filter coefficients.
target_link_libraries(pulp-signal INTERFACE pulp::music pulp::foundation pulp::timebase)

# Vectorized block kernels (dot products, reductions, FIR correlation). A
# compiled library, but deliberately not pulp::runtime: its only link item is
# Highway, so the TLS-free link line above is preserved.
target_link_libraries(pulp-signal INTERFACE pulp::simd)

# Link Accelerate framework on Apple for vDSP FFT optimization
if(APPLE)
    target_link_libraries(pulp-signal INTERFACE "-framework Accelerate")
endif()

# Float FFT backend for FftT off Apple: muFFT (MIT), SIMD-dispatched at run
# time on x86-64 (SSE, SSE3, AVX) and portable C everywhere else. Apple keeps
# vDSP and builds this only so tests can exercise the portable backend on any
# host. Pulp compiles muFFT's sources itself plus a C shim, so muFFT's header
# never reaches fft.hpp, and muFFT's own CMake (FFTW-linked bench and test
# binaries) never runs. FftT<double> stays on Pulp's built-in transform.
if(DEFINED mufft_SOURCE_DIR)
    set(_pulp_mufft_core
        ${mufft_SOURCE_DIR}/fft.c ${mufft_SOURCE_DIR}/kernel.c ${mufft_SOURCE_DIR}/cpu.c)
    set(_pulp_mufft_x86 FALSE)
    if(NOT EMSCRIPTEN AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_pulp_mufft_x86 TRUE)
    endif()
    # muFFT's allocations are routed through counting wrappers in the shim
    # (see fft_mufft_alloc_hooks.h) so tests can prove transforms allocate
    # nothing.
    set(_pulp_mufft_hooks ${CMAKE_CURRENT_SOURCE_DIR}/src/fft_mufft_alloc_hooks.h)
    if(MSVC)
        set(_pulp_mufft_force_include "/FI${_pulp_mufft_hooks}")
    else()
        set(_pulp_mufft_force_include "-include;${_pulp_mufft_hooks}")
    endif()
    set(_pulp_mufft_defs "")
    if(_pulp_mufft_x86)
        set(_pulp_mufft_defs MUFFT_HAVE_X86 MUFFT_HAVE_SSE MUFFT_HAVE_SSE3 MUFFT_HAVE_AVX)
    endif()

    # muFFT is C99 written for GCC/Clang. Prove it compiles for this target
    # before depending on it; if it does not (an untested compiler such as
    # MSVC), FftT<float> keeps Pulp's built-in transform.
    set(_pulp_mufft_probe_defs "")
    foreach(_d IN LISTS _pulp_mufft_defs)
        list(APPEND _pulp_mufft_probe_defs "-D${_d}")
    endforeach()
    set(_pulp_saved_try_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(PULP_MUFFT_COMPILES
        ${CMAKE_CURRENT_BINARY_DIR}/mufft-probe
        SOURCES ${_pulp_mufft_core} ${CMAKE_CURRENT_SOURCE_DIR}/src/fft_mufft.c
        COMPILE_DEFINITIONS ${_pulp_mufft_probe_defs}
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${mufft_SOURCE_DIR}"
        C_STANDARD 99
        OUTPUT_VARIABLE _pulp_mufft_probe_log)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_pulp_saved_try_type}")
    if(NOT PULP_MUFFT_COMPILES)
        string(SUBSTRING "${_pulp_mufft_probe_log}" 0 2000 _pulp_mufft_probe_head)
        message(STATUS "Pulp: muFFT does not compile for this target; "
                       "float FFTs use the built-in transform. Compiler output:\n"
                       "${_pulp_mufft_probe_head}")
    endif()
endif()

if(DEFINED mufft_SOURCE_DIR AND PULP_MUFFT_COMPILES)
    add_library(pulp-signal-mufft STATIC src/fft_mufft.c ${_pulp_mufft_core})
    add_library(pulp::signal-mufft ALIAS pulp-signal-mufft)
    target_include_directories(pulp-signal-mufft PRIVATE ${mufft_SOURCE_DIR})
    target_compile_definitions(pulp-signal-mufft PRIVATE ${_pulp_mufft_defs})
    set_target_properties(pulp-signal-mufft PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                                     POSITION_INDEPENDENT_CODE ON)
    set_source_files_properties(${_pulp_mufft_core} PROPERTIES
        COMPILE_OPTIONS "${_pulp_mufft_force_include}")
    if(NOT MSVC)
        target_link_libraries(pulp-signal-mufft PRIVATE m)
    endif()
    if(_pulp_mufft_x86)
        # Each kernel file is compiled for its own instruction set; cpu.c picks
        # one at run time from CPUID, so the rest of Pulp stays at the baseline.
        set(_sse ${mufft_SOURCE_DIR}/x86/kernel.sse.c)
        set(_sse3 ${mufft_SOURCE_DIR}/x86/kernel.sse3.c)
        set(_avx ${mufft_SOURCE_DIR}/x86/kernel.avx.c)
        target_sources(pulp-signal-mufft PRIVATE ${_sse} ${_sse3} ${_avx})
        if(MSVC)
            set_source_files_properties(${_sse} ${_sse3} PROPERTIES
                COMPILE_OPTIONS "${_pulp_mufft_force_include}")
            set_source_files_properties(${_avx} PROPERTIES
                COMPILE_OPTIONS "${_pulp_mufft_force_include};/arch:AVX")
        else()
            set_source_files_properties(${_sse} PROPERTIES
                COMPILE_OPTIONS "${_pulp_mufft_force_include};-msse;-mno-sse3;-mno-avx")
            set_source_files_properties(${_sse3} PROPERTIES
                COMPILE_OPTIONS "${_pulp_mufft_force_include};-msse;-msse3;-mno-avx")
            set_source_files_properties(${_avx} PROPERTIES
                COMPILE_OPTIONS "${_pulp_mufft_force_include};-msse;-msse3;-mavx")
        endif()
    endif()
    if(NOT APPLE)
        target_link_libraries(pulp-signal INTERFACE pulp-signal-mufft)
        target_compile_definitions(pulp-signal INTERFACE PULP_FFT_HAS_MUFFT=1)
    endif()
endif()

# Multi-backend FFT (vdsp / kissfft / fftw3 / mkl). Source-bearing static
# library separate from the INTERFACE umbrella so callers opt in only when
# they need the runtime-selectable wrapper. FFTW3 and MKL are loaded via
# dlopen at runtime — no link-time dependency, no SDK requirement at build.
add_library(pulp-signal-fft-backend STATIC
    src/fft_backend.cpp
)
add_library(pulp::signal-fft-backend ALIAS pulp-signal-fft-backend)
target_include_directories(pulp-signal-fft-backend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(pulp-signal-fft-backend PUBLIC pulp::signal)
if(APPLE)
    target_link_libraries(pulp-signal-fft-backend PUBLIC "-framework Accelerate")
endif()
if(NOT WIN32)
    target_link_libraries(pulp-signal-fft-backend PUBLIC ${CMAKE_DL_LIBS})
endif()

# Modal spec load/save. Source-bearing static library rather than part of the
# INTERFACE umbrella because it is the only TU that parses JSON: keeping choc
# PRIVATE here stops a choc include dir propagating into every DSP translation
# unit that pulls in pulp::signal.
add_library(pulp-signal-modal-spec STATIC
    src/modal_spec.cpp
)
add_library(pulp::signal-modal-spec ALIAS pulp-signal-modal-spec)
target_include_directories(pulp-signal-modal-spec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)
target_link_libraries(pulp-signal-modal-spec PUBLIC pulp::signal)
