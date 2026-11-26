#include <pulp/vellum_audio/audio_display_provider.hpp>
#include <pulp/vellum_audio/waveform_editor.hpp>

#include <array>

namespace pulp::vellum_audio {

void WaveformEditor::set_audio_provider(AudioDisplayProvider* provider, std::size_t channel) {
    std::array<float, AudioDisplayProvider::kMaxScopeSamples> samples{};
    AudioFrameInfo info;
    const auto count = provider ? provider->read_vector(channel, samples, info) : 0;
    set_audio_data(count == 0 ? nullptr : samples.data(), static_cast<int>(count),
                   static_cast<float>(info.sample_rate));
}

} // namespace pulp::vellum_audio
