add_library(pulp-signal INTERFACE)
add_library(pulp::signal ALIAS pulp-signal)

target_include_directories(pulp-signal
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# pulp/signal/convolver_messages.hpp transitively includes
# pulp::runtime's SpscQueue (lock-free retired-IR ring). Propagate
# the dependency so consumers of pulp::signal don't have to know.
target_link_libraries(pulp-signal INTERFACE pulp::runtime)

# Link Accelerate framework on Apple for vDSP FFT optimization
if(APPLE)
    target_link_libraries(pulp-signal INTERFACE "-framework Accelerate")
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
