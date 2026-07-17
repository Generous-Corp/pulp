// wav_bridge.cpp — write an offline render to a WAV file. Encoding is delegated
// to pulp::audio::write_wav_file (CHOC); this only deinterleaves the harness's
// owning Buffer into the AudioFileData that writer expects.

#include "wav_bridge.hpp"

#include <cmath>
#include <cstdint>

namespace pulp::test::audio {

bool write_buffer_wav(const pulp::audio::Buffer<float>& buffer,
                      double sample_rate, const std::string& path,
                      pulp::audio::WavBitDepth bit_depth) {
    if (buffer.num_channels() == 0 || buffer.num_samples() == 0)
        return false;
    if (!(sample_rate > 0.0))
        return false;

    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<std::uint32_t>(std::llround(sample_rate));
    data.channels.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        const auto samples = buffer.channel(ch);
        data.channels[ch].assign(samples.begin(), samples.end());
    }
    return pulp::audio::write_wav_file(path, data, bit_depth);
}

bool write_scenario_wav(const ScenarioResult& result, const std::string& path,
                        pulp::audio::WavBitDepth bit_depth) {
    return write_buffer_wav(result.output, result.sample_rate, path, bit_depth);
}

} // namespace pulp::test::audio
