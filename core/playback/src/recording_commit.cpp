#include <pulp/playback/recording_commit.hpp>

#include <pulp/runtime/crypto.hpp>

#include <bit>
#include <limits>

namespace pulp::playback {
namespace {

template <typename T>
runtime::Result<SealedRecordingTake, RecordingCommitError> failure(RecordingCommitError error) {
    return runtime::Result<SealedRecordingTake, RecordingCommitError>(runtime::Err(error));
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

void append_tag(std::vector<std::uint8_t>& bytes, const char (&tag)[5]) {
    bytes.insert(bytes.end(), tag, tag + 4);
}

bool valid_request(const audio::BufferView<const float>& audio,
                   const RecordingTakeCommitRequest& request) noexcept {
    const auto sample_rate = request.sample_rate.normalized();
    return request.sequence_id.valid() && request.track_id.valid() &&
           request.take_lane_id.valid() && request.asset_id.valid() && request.take_id.valid() &&
           sample_rate.denominator == 1 && sample_rate.numerator > 0 &&
           request.asset_name.size() > 0 && audio.num_channels() > 0 && audio.num_samples() > 0 &&
           (!request.create_take_lane || !request.take_lane_name.empty());
}

runtime::Result<std::vector<std::uint8_t>, RecordingCommitError>
encode_float_wave(const audio::BufferView<const float>& audio, timebase::RationalRate sample_rate) {
    if (audio.num_channels() > std::numeric_limits<std::uint16_t>::max() / sizeof(float) ||
        sample_rate.numerator > std::numeric_limits<std::uint32_t>::max())
        return runtime::Result<std::vector<std::uint8_t>, RecordingCommitError>(
            runtime::Err(RecordingCommitError::UnsupportedWaveShape));
    const auto channels = static_cast<std::uint64_t>(audio.num_channels());
    const auto frames = static_cast<std::uint64_t>(audio.num_samples());
    if (channels > std::numeric_limits<std::uint64_t>::max() / frames ||
        channels * frames >
            std::numeric_limits<std::uint32_t>::max() / static_cast<std::uint64_t>(sizeof(float)))
        return runtime::Result<std::vector<std::uint8_t>, RecordingCommitError>(
            runtime::Err(RecordingCommitError::WaveSizeExceeded));
    const auto data_bytes =
        static_cast<std::uint32_t>(channels * frames * static_cast<std::uint64_t>(sizeof(float)));
    if (data_bytes > std::numeric_limits<std::uint32_t>::max() - 36)
        return runtime::Result<std::vector<std::uint8_t>, RecordingCommitError>(
            runtime::Err(RecordingCommitError::WaveSizeExceeded));

    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + data_bytes);
    append_tag(bytes, "RIFF");
    append_u32(bytes, 36 + data_bytes);
    append_tag(bytes, "WAVE");
    append_tag(bytes, "fmt ");
    append_u32(bytes, 16);
    append_u16(bytes, 3);
    append_u16(bytes, static_cast<std::uint16_t>(channels));
    append_u32(bytes, static_cast<std::uint32_t>(sample_rate.numerator));
    const auto block_align = static_cast<std::uint16_t>(channels * sizeof(float));
    if (sample_rate.numerator > std::numeric_limits<std::uint32_t>::max() / block_align)
        return runtime::Result<std::vector<std::uint8_t>, RecordingCommitError>(
            runtime::Err(RecordingCommitError::UnsupportedWaveShape));
    append_u32(bytes, static_cast<std::uint32_t>(sample_rate.numerator) * block_align);
    append_u16(bytes, block_align);
    append_u16(bytes, 32);
    append_tag(bytes, "data");
    append_u32(bytes, data_bytes);
    for (std::size_t frame = 0; frame < audio.num_samples(); ++frame) {
        for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
            append_u32(bytes, std::bit_cast<std::uint32_t>(audio.channel(channel)[frame]));
    }
    return runtime::Result<std::vector<std::uint8_t>, RecordingCommitError>(
        runtime::Ok(std::move(bytes)));
}

} // namespace

runtime::Result<SealedRecordingTake, RecordingCommitError>
seal_recording_take(const audio::BufferView<const float>& audio,
                    RecordingTakeCommitRequest request) {
    if (!valid_request(audio, request))
        return failure<SealedRecordingTake>(RecordingCommitError::InvalidRequest);
    request.sample_rate = request.sample_rate.normalized();
    auto encoded = encode_float_wave(audio, request.sample_rate);
    if (!encoded)
        return runtime::Result<SealedRecordingTake, RecordingCommitError>(
            runtime::Err(encoded.error()));
    auto wav_bytes = std::move(encoded).value();
    const auto hash_hex = runtime::sha256_hex(wav_bytes.data(), wav_bytes.size());
    const auto hash = timeline::ContentHash::from_hex(hash_hex);
    if (!hash)
        return failure<SealedRecordingTake>(RecordingCommitError::HashFailure);

    timeline::MediaAsset asset;
    asset.id = request.asset_id;
    asset.name = std::move(request.asset_name);
    asset.frame_count = audio.num_samples();
    asset.sample_rate = request.sample_rate;
    asset.content_hash = *hash;
    asset.storage_policy = timeline::AssetStoragePolicy::Embedded;
    asset.locators.push_back(
        {timeline::AssetLocatorKind::PackageRelative, "media/" + hash_hex + ".wav"});

    auto take = timeline::Take::create(request.take_id, {request.asset_id, {}, audio.num_samples()},
                                       request.placement_start, request.sample_rate);
    if (!take)
        return failure<SealedRecordingTake>(RecordingCommitError::InvalidTake);

    SealedRecordingTake result{
        asset,
        std::move(take).value(),
        std::move(wav_bytes),
        {},
    };
    result.commands.emplace_back(timeline::CreateAsset{result.asset});
    if (request.create_take_lane) {
        auto lane = timeline::TakeLane::create(request.take_lane_id,
                                               std::move(request.take_lane_name), {result.take});
        if (!lane)
            return failure<SealedRecordingTake>(RecordingCommitError::InvalidTakeLane);
        result.commands.emplace_back(timeline::InsertTakeLane{
            request.sequence_id,
            request.track_id,
            std::move(lane).value(),
        });
    } else {
        result.commands.emplace_back(timeline::InsertTake{
            request.sequence_id,
            request.track_id,
            request.take_lane_id,
            result.take,
        });
    }
    return runtime::Result<SealedRecordingTake, RecordingCommitError>(
        runtime::Ok(std::move(result)));
}

runtime::Result<SealedRecordingTake, RecordingCommitError>
seal_retrospective_take(const audio::RollingAudioCaptureBuffer& capture,
                        const audio::RollingAudioCaptureHold& hold,
                        RecordingTakeCommitRequest request) {
    if (!hold.valid() || hold.snapshot().num_channels == 0 || hold.snapshot().frame_count == 0)
        return failure<SealedRecordingTake>(RecordingCommitError::RetrospectiveUnavailable);
    audio::Buffer<float> materialized(hold.snapshot().num_channels, hold.snapshot().frame_count);
    if (capture.materialize_held(hold, materialized.view()).status !=
        audio::RollingAudioCaptureMaterializeStatus::Ok)
        return failure<SealedRecordingTake>(RecordingCommitError::RetrospectiveUnavailable);
    return seal_recording_take(static_cast<const audio::Buffer<float>&>(materialized).view(),
                               std::move(request));
}

} // namespace pulp::playback
