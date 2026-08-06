add_library(pulp-smf-interop
    src/smf_decode.cpp
    src/smf_import.cpp
    src/smf_export.cpp
)
add_library(pulp::smf-interop ALIAS pulp-smf-interop)

target_compile_features(pulp-smf-interop PUBLIC cxx_std_20)

# The raw codec compiles against the generated policy table so a widened row
# cannot outpace its implementation. Keep that dependency private: users of
# pulp::smf-interop still depend only on the timeline API.
target_include_directories(pulp-smf-interop PRIVATE
    ${PROJECT_SOURCE_DIR}/core/interchange/include
)

# Standard MIDI File interop is foreign-format I/O, so it links the timeline
# model but stays out of the dependency-minimal pulp::timeline target. It needs
# no MIDI transport, device, or realtime buffer surface: an SMF is decoded from
# bytes straight into timeline ticks, so pulp::midi is deliberately not a
# dependency here.
target_link_libraries(pulp-smf-interop
    PUBLIC
        pulp::timeline
    PRIVATE
        pulp::runtime
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-smf-interop PRIVATE -fno-exceptions -fno-rtti)
endif()

# Consent-gated interchange adapter. Keep it separate so the established
# pulp::smf-interop installed component does not acquire an interchange link
# edge merely because the agent-facing export surface exists.
add_library(pulp-smf-interchange
    src/smf_interchange.cpp
)
add_library(pulp::smf-interchange ALIAS pulp-smf-interchange)

target_compile_features(pulp-smf-interchange PUBLIC cxx_std_20)

target_include_directories(pulp-smf-interchange PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(pulp-smf-interchange
    PUBLIC
        pulp::interchange
        pulp::smf-interop
    PRIVATE
        pulp::runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-smf-interchange PRIVATE -fno-exceptions -fno-rtti)
endif()
