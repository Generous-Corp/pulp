# trace-plugin-chain: profile a REAL effect chain (PulpGain -> PulpEffect ->
# PulpCompressor) offline and reveal, via Perfetto `dsp.node` spans, which
# plugin's process() dominates the block. Perfetto-only, so gated on the
# dev-only PULP_TRACING option (see examples/CMakeLists.txt). GPU-free.

add_executable(pulp-trace-plugin-chain
    main.cpp
    gain.cpp
    filter.cpp
    compressor.cpp)

target_link_libraries(pulp-trace-plugin-chain
    PRIVATE pulp::audio pulp::runtime pulp::format)

# Each wrapper TU includes exactly one plugin's header; the driver includes
# none of them. The colliding unscoped enums never share a translation unit.
target_include_directories(pulp-trace-plugin-chain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../pulp-gain
    ${CMAKE_CURRENT_SOURCE_DIR}/../pulp-effect
    ${CMAKE_CURRENT_SOURCE_DIR}/../pulp-compressor)
