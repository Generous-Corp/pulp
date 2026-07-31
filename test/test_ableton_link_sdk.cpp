#include <pulp/playback/tempo_sync.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>

#if defined(PULP_HAS_ABLETON_LINK)
#include <pulp/playback/ableton_link.hpp>
#endif

int main() {
#if !defined(PULP_HAS_ABLETON_LINK)
    std::cerr << "SKIP: desktop Ableton Link SDK is absent; configure with "
                 "-DPULP_ENABLE_ABLETON_LINK=ON and "
                 "-DPULP_ABLETON_LINK_SDK_DIR=/absolute/out-of-tree/link\n";
    return 77;
#else
    try {
        try {
            pulp::playback::AbletonLinkTempoSync invalid(std::numeric_limits<double>::quiet_NaN());
            std::cerr << "desktop Ableton Link adapter accepted an invalid initial tempo\n";
            return 1;
        } catch (const std::invalid_argument&) {
        }

        pulp::playback::AbletonLinkTempoSync source(120.0);
        source.set_start_stop_sync_enabled(true);
        source.set_enabled(true);

        const std::array points{pulp::timebase::TempoPoint{{0}, 120.0}};
        auto compiled = pulp::timebase::CompiledTempoMap::compile(
            points, pulp::timebase::RationalRate{48'000, 1});
        if (!compiled) {
            std::cerr << "could not compile Link smoke-test tempo map\n";
            return 1;
        }
        pulp::playback::MasterTransport transport;
        pulp::playback::MasterTransportConfig config;
        config.max_buffer_size = 64;
        config.tempo_sync_source = &source;
        if (transport.prepare(compiled.value(), config) != pulp::playback::TransportError::None ||
            transport.set_playing(true) != pulp::playback::TransportError::None) {
            std::cerr << "could not prepare Link-backed transport\n";
            return 1;
        }
        pulp::playback::TempoSyncHostTime output_time;
        if (source.output_host_time(64, 48'000.0, output_time) !=
            pulp::playback::TempoSyncError::None) {
            std::cerr << "desktop Ableton Link adapter could not produce output host time\n";
            return 1;
        }
        pulp::playback::TransportSnapshot state;
        const auto result = transport.begin_block(64, output_time, state);
        source.set_enabled(false);
        if (result != pulp::playback::TransportError::None ||
            !pulp::playback::valid_transport_ranges(state)) {
            std::cerr << "desktop Ableton Link adapter returned an invalid block state\n";
            return 1;
        }
        if (!state.is_playing || !state.ranges[0].host_beat_mapping) {
            std::cerr << "desktop Ableton Link adapter did not start on its own clock\n";
            return 1;
        }
        std::cout << "desktop Ableton Link adapter available; peers=" << source.peer_count()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "desktop Ableton Link adapter construction failed: " << error.what() << '\n';
        return 1;
    }
#endif
}
