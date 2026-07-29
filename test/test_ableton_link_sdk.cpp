#include <pulp/playback/tempo_sync.hpp>

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

        pulp::playback::TempoSyncBlockRequest request;
        request.frame_count = 64;
        request.sample_rate = 48'000.0;
        request.quantum_beats = 4.0;
        pulp::playback::TempoSyncBlockState state;
        const auto result = source.capture_audio_block(request, state);
        source.set_enabled(false);
        if (result != pulp::playback::TempoSyncError::None ||
            !pulp::playback::valid_tempo_sync_state(state)) {
            std::cerr << "desktop Ableton Link adapter returned an invalid block state\n";
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
