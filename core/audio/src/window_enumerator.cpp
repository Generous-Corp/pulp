#include <pulp/audio/window_enumerator.hpp>

namespace pulp::audio {

WindowEnumerationResult enumerate_excerpt_windows(const std::string& source_path,
                                                  const AudioFileData& audio,
                                                  const ExcerptQuery& query) {
    WindowEnumerationResult result;
    result.source_frames = audio.num_frames();

    if (query.window_frames == 0 || query.hop_frames == 0) {
        result.status = WindowEnumerationStatus::invalid_query;
        return result;
    }

    if (audio.sample_rate == 0 || audio.empty()) {
        result.status = WindowEnumerationStatus::empty_audio;
        return result;
    }

    if (result.source_frames < query.window_frames) {
        result.status = WindowEnumerationStatus::file_too_short;
        return result;
    }

    const auto last_valid_start = result.source_frames - query.window_frames;
    uint64_t start = 0;
    while (start <= last_valid_start) {
        result.windows.push_back({
            .source_path = source_path,
            .sample_rate = audio.sample_rate,
            .start_frame = start,
            .frame_count = query.window_frames,
        });

        const auto remaining_frames = last_valid_start - start;
        if (remaining_frames <= query.hop_frames) {
            break;
        }
        start += query.hop_frames;
    }

    if (result.windows.empty() || result.windows.back().start_frame != last_valid_start) {
        result.windows.push_back({
            .source_path = source_path,
            .sample_rate = audio.sample_rate,
            .start_frame = last_valid_start,
            .frame_count = query.window_frames,
        });
    }

    result.status = WindowEnumerationStatus::ok;
    return result;
}

} // namespace pulp::audio
