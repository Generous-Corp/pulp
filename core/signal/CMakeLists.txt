add_library(pulp-signal INTERFACE)
add_library(pulp::signal ALIAS pulp-signal)

target_include_directories(pulp-signal
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link Accelerate framework on Apple for vDSP FFT optimization
if(APPLE)
    target_link_libraries(pulp-signal INTERFACE "-framework Accelerate")
endif()
