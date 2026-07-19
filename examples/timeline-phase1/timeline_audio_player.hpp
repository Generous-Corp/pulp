#pragma once

#include "timeline_example_engine.hpp"

#include <pulp/audio/wav_decoder.hpp>
#include <pulp/timeline/assets.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::examples::timeline_phase1 {

/// Basic Phase-1 audio-file player: a real Project/Sequence/Track/MediaRef is
/// compiled into an arrangement audio renderer and lowered into SignalGraph.
/// WAV parsing uses the bounded no-exceptions production decoder.
class TimelineAudioPlayerProcessor final : public format::Processor {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    explicit TimelineAudioPlayerProcessor(std::span<const std::uint8_t> wav_bytes,
                                          audio::WavDecodeLimits limits = {});

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext& context) override;
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override;

    bool source_valid() const noexcept { return decoded_ != nullptr; }
    bool engine_prepared() const noexcept { return engine_.prepared(); }
    playback::TransportError set_playing(bool playing) noexcept;
    playback::TransportError seek_samples(std::int64_t sample) noexcept;
    playback::TransportError set_loop_samples(bool enabled, std::int64_t start,
                                              std::int64_t end) noexcept;
    const playback::TransportSnapshot& last_transport() const noexcept;

  private:
    std::shared_ptr<const audio::AudioFileData> decoded_;
    std::optional<timeline::ContentHash> content_hash_;
    TimelineExampleEngine engine_;
};

/// Small deterministic PCM16 WAV used only by the headless validation mode and
/// tests. The normal standalone path reads the user-supplied WAV file.
std::vector<std::uint8_t> make_timeline_audio_validation_wav(std::uint32_t frames = 512);
std::unique_ptr<format::Processor> create_validation_timeline_audio_player();

} // namespace pulp::examples::timeline_phase1
