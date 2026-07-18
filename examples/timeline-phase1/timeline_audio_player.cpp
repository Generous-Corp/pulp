#include "timeline_audio_player.hpp"

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace pulp::examples::timeline_phase1 {
namespace {

template <class T, class E>
std::optional<T> value_or_none(runtime::Result<T, E> result) {
    if (!result)
        return std::nullopt;
    return std::move(result).value();
}

std::shared_ptr<const timeline::Project>
make_audio_project(const audio::AudioFileData& decoded,
                   const timeline::ContentHash& content_hash) {
    const timebase::RationalRate source_rate{decoded.sample_rate, 1};
    const auto frame_count = decoded.num_frames();
    auto clip = value_or_none(timeline::Clip::create_absolute(
        {5}, {0}, frame_count, source_rate, timeline::MediaRef{{2}, {0}, frame_count}));
    if (!clip)
        return {};
    auto track = value_or_none(
        timeline::Track::create({4}, "Audio file", {std::move(*clip)}));
    if (!track)
        return {};
    auto sequence = value_or_none(timeline::Sequence::create(
        {3}, "Player", std::nullopt,
        timeline::AbsoluteTimelineDuration{frame_count, source_rate},
        std::vector<timeline::Track>{std::move(*track)}));
    if (!sequence)
        return {};
    timeline::MediaAsset asset;
    asset.id = {2};
    asset.name = "Loaded WAV";
    asset.frame_count = frame_count;
    asset.sample_rate = source_rate;
    asset.content_hash = content_hash;

    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Timeline audio player";
    input.next_item_id = 6;
    input.root_sequence_id = {3};
    input.assets = {std::move(asset)};
    input.sequences = {std::move(*sequence)};
    auto project = value_or_none(timeline::Project::create(std::move(input)));
    return project ? std::make_shared<const timeline::Project>(std::move(*project)) : nullptr;
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    append_u16(bytes, static_cast<std::uint16_t>(value));
    append_u16(bytes, static_cast<std::uint16_t>(value >> 16));
}
void append_fourcc(std::vector<std::uint8_t>& bytes, const char (&text)[5]) {
    bytes.insert(bytes.end(), text, text + 4);
}

} // namespace

TimelineAudioPlayerProcessor::TimelineAudioPlayerProcessor(
    std::span<const std::uint8_t> wav_bytes, audio::WavDecodeLimits limits) {
    content_hash_ = timeline::ContentHash::from_hex(
        runtime::sha256_hex(wav_bytes.data(), wav_bytes.size()));
    auto decoded = playback::DecodedAudioAssetPool::decode_wav({2}, wav_bytes, limits);
    if (decoded && content_hash_)
        decoded_ = std::move(decoded).value().audio;
}

format::PluginDescriptor TimelineAudioPlayerProcessor::descriptor() const {
    return {.name = "Timeline Audio Player",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.timeline-audio-player",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0};
}

void TimelineAudioPlayerProcessor::prepare(const format::PrepareContext& context) {
    if (!decoded_ || context.sample_rate <= 0.0 || context.max_buffer_size <= 0)
        return;
    auto project = make_audio_project(*decoded_, *content_hash_);
    if (!project)
        return;
    const auto integer_rate = static_cast<std::uint64_t>(std::llround(context.sample_rate));
    if (integer_rate == 0)
        return;
    const std::array points{timebase::TempoPoint{{0}, 120.0}};
    auto map = std::make_shared<const timebase::CompiledTempoMap>(
        points, timebase::RationalRate{integer_rate, 1});
    auto pool = playback::DecodedAudioAssetPool::create({{{2}, decoded_}});
    if (!pool)
        return;

    playback::ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {3};
    request.tempo_map = std::move(map);
    request.document_revision = 1;
    request.audio_assets = std::move(pool).value();
    request.audio_limits.max_channels = 2;
    request.audio_limits.max_block_frames =
        static_cast<std::uint32_t>(context.max_buffer_size);
    engine_.prepare(std::move(request), context.sample_rate,
                    static_cast<std::uint32_t>(context.max_buffer_size), false);
}

void TimelineAudioPlayerProcessor::process(audio::BufferView<float>& output,
                                           const audio::BufferView<const float>& input,
                                           midi::MidiBuffer&, midi::MidiBuffer&,
                                           const format::ProcessContext&) {
    engine_.process(output, input);
}

playback::TransportError TimelineAudioPlayerProcessor::set_playing(bool playing) noexcept {
    return engine_.set_playing(playing);
}
playback::TransportError TimelineAudioPlayerProcessor::seek_samples(std::int64_t sample) noexcept {
    return engine_.seek_samples(sample);
}
playback::TransportError TimelineAudioPlayerProcessor::set_loop_samples(
    bool enabled, std::int64_t start, std::int64_t end) noexcept {
    return engine_.set_loop_samples(enabled, start, end);
}
const playback::TransportSnapshot& TimelineAudioPlayerProcessor::last_transport() const noexcept {
    return engine_.last_transport();
}

std::vector<std::uint8_t> make_timeline_audio_validation_wav(std::uint32_t frames) {
    constexpr std::uint32_t kRate = 48'000;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBits = 16;
    const std::uint32_t data_bytes = frames * sizeof(std::int16_t);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + data_bytes);
    append_fourcc(bytes, "RIFF");
    append_u32(bytes, 36 + data_bytes);
    append_fourcc(bytes, "WAVE");
    append_fourcc(bytes, "fmt ");
    append_u32(bytes, 16);
    append_u16(bytes, 1);
    append_u16(bytes, kChannels);
    append_u32(bytes, kRate);
    append_u32(bytes, kRate * kChannels * kBits / 8);
    append_u16(bytes, kChannels * kBits / 8);
    append_u16(bytes, kBits);
    append_fourcc(bytes, "data");
    append_u32(bytes, data_bytes);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto sample = static_cast<std::int16_t>(
            std::sin(static_cast<double>(frame) * 0.071) * 20'000.0);
        append_u16(bytes, static_cast<std::uint16_t>(sample));
    }
    return bytes;
}

std::unique_ptr<format::Processor> create_validation_timeline_audio_player() {
    const auto wav = make_timeline_audio_validation_wav();
    return std::make_unique<TimelineAudioPlayerProcessor>(wav);
}

} // namespace pulp::examples::timeline_phase1
