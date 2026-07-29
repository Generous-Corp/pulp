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
