#include <pulp/audio/sample_heritage_record_commit.hpp>

#include <pulp/audio/sample_heritage_voice_dsp.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/signal/resampler.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::audio {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kMetadataSchema =
    "pulp.sample-heritage-committed-asset.v1";

using JsonValue = choc::value::ValueView;

SampleHeritageRecordCommitResult fail(SampleHeritageRecordCommitStatus status,
                                      std::string detail) {
    SampleHeritageRecordCommitResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

bool valid_text(std::string_view value) {
    return !value.empty() && value.size() <= 4096;
}

bool valid_provenance(const SampleHeritageRecordProvenance& provenance) {
    return valid_text(provenance.source_id) &&
           valid_text(provenance.capture_method) &&
           valid_text(provenance.evidence_id);
}

bool valid_sha256(std::string_view value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::string audio_sha256(BufferView<const float> audio, double sample_rate) {
    constexpr std::string_view domain = "pulp.sample-heritage.audio.v1";
    const auto samples = audio.num_channels() * audio.num_samples();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(domain.size() + 24 + samples * sizeof(float));
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    append_u64(bytes, std::bit_cast<std::uint64_t>(sample_rate));
    append_u32(bytes, static_cast<std::uint32_t>(audio.num_channels()));
    append_u64(bytes, static_cast<std::uint64_t>(audio.num_samples()));
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel) {
        for (float sample : audio.channel(channel)) {
            if (sample == 0.0f) sample = 0.0f;
            append_u32(bytes, std::bit_cast<std::uint32_t>(sample));
        }
    }
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

bool copy_audio(BufferView<const float> source, Buffer<float>& destination) {
    destination.resize(source.num_channels(), source.num_samples());
    for (std::size_t channel = 0; channel < source.num_channels(); ++channel)
        std::copy(source.channel(channel).begin(), source.channel(channel).end(),
                  destination.channel(channel).begin());
    return true;
}

void append_number(std::string& json, double value) {
    if (value == 0.0) value = 0.0;
    std::array<char, 64> buffer{};
    const auto conversion =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (conversion.ec != std::errc{}) throw std::runtime_error("number encoding failed");
    json.append(buffer.data(), conversion.ptr);
}

void append_string(std::string& json, std::string_view value) {
    json += choc::json::getEscapedQuotedString(value);
}

std::string write_metadata(const SampleHeritageCommittedAssetMetadata& metadata) {
    std::string json;
    json.reserve(1024);
    json += "{\"schema\":\"pulp.sample-heritage-committed-asset.v1\",\"profile\":{";
    json += "\"schema_version\":" + std::to_string(metadata.profile_schema_version);
    json += ",\"id\":";
    append_string(json, metadata.profile_id);
    json += ",\"digest_sha256\":";
    append_string(json, metadata.profile_digest_sha256);
    json += "},\"source\":{\"sample_rate\":";
    append_number(json, metadata.source_sample_rate);
    json += ",\"frames\":" + std::to_string(metadata.source_frames);
    json += ",\"channels\":" + std::to_string(metadata.source_channels);
    json += ",\"audio_sha256\":";
    append_string(json, metadata.source_audio_sha256);
    json += "},\"committed\":{\"sample_rate\":";
    append_number(json, metadata.committed_sample_rate);
    json += ",\"frames\":" + std::to_string(metadata.committed_frames);
    json += ",\"channels\":" + std::to_string(metadata.committed_channels);
    json += ",\"audio_sha256\":";
    append_string(json, metadata.committed_audio_sha256);
    json += "},\"provenance\":{\"source_id\":";
    append_string(json, metadata.provenance.source_id);
    json += ",\"capture_method\":";
    append_string(json, metadata.provenance.capture_method);
    json += ",\"evidence_id\":";
    append_string(json, metadata.provenance.evidence_id);
    json += "}}";
    return json;
}

template <std::size_t Size>
bool audit_object(JsonValue object,
                  const std::array<std::string_view, Size>& expected) {
    if (!object.isObject() || object.size() != Size) return false;
    for (std::uint32_t index = 0; index < object.size(); ++index) {
        const auto member = object.getObjectMemberAt(index);
        if (std::find(expected.begin(), expected.end(), member.name) == expected.end())
            return false;
        for (std::uint32_t earlier = 0; earlier < index; ++earlier)
            if (object.getObjectMemberAt(earlier).name == member.name) return false;
    }
    return std::all_of(expected.begin(), expected.end(),
                       [&](auto name) { return object.hasObjectMember(name); });
}

bool read_string(JsonValue object, std::string_view name, std::string& destination) {
    const auto value = object[name];
    if (!value.isString()) return false;
    destination = value.getString();
    return true;
}

template <typename Integer>
bool read_integer(JsonValue object, std::string_view name, Integer& destination) {
    const auto value = object[name];
    if (!value.isInt()) return false;
    const auto input = value.get<std::int64_t>();
    if (input < 0 || static_cast<std::uint64_t>(input) >
                         static_cast<std::uint64_t>(std::numeric_limits<Integer>::max()))
        return false;
    destination = static_cast<Integer>(input);
    return true;
}

bool read_number(JsonValue object, std::string_view name, double& destination) {
    const auto value = object[name];
    if (value.isInt())
        destination = static_cast<double>(value.get<std::int64_t>());
    else if (value.isFloat())
        destination = value.get<double>();
    else
        return false;
    return std::isfinite(destination);
}

bool parse_metadata(std::string_view json,
                    SampleHeritageCommittedAssetMetadata& metadata) {
    try {
        const auto owner = choc::json::parse(json);
        const auto root = JsonValue(owner);
        constexpr std::array root_fields{"schema"sv, "profile"sv, "source"sv,
                                         "committed"sv, "provenance"sv};
        if (!audit_object(root, root_fields) || !root["schema"].isString() ||
            root["schema"].getString() != kMetadataSchema)
            return false;

        const auto profile = root["profile"];
        constexpr std::array profile_fields{"schema_version"sv, "id"sv,
                                            "digest_sha256"sv};
        if (!audit_object(profile, profile_fields) ||
            !read_integer(profile, "schema_version", metadata.profile_schema_version) ||
            !read_string(profile, "id", metadata.profile_id) ||
            !read_string(profile, "digest_sha256", metadata.profile_digest_sha256))
            return false;

        const auto source = root["source"];
        constexpr std::array audio_fields{"sample_rate"sv, "frames"sv, "channels"sv,
                                          "audio_sha256"sv};
        if (!audit_object(source, audio_fields) ||
            !read_number(source, "sample_rate", metadata.source_sample_rate) ||
            !read_integer(source, "frames", metadata.source_frames) ||
            !read_integer(source, "channels", metadata.source_channels) ||
            !read_string(source, "audio_sha256", metadata.source_audio_sha256))
            return false;

        const auto committed = root["committed"];
        if (!audit_object(committed, audio_fields) ||
            !read_number(committed, "sample_rate", metadata.committed_sample_rate) ||
            !read_integer(committed, "frames", metadata.committed_frames) ||
            !read_integer(committed, "channels", metadata.committed_channels) ||
            !read_string(committed, "audio_sha256", metadata.committed_audio_sha256))
            return false;

        const auto provenance = root["provenance"];
        constexpr std::array provenance_fields{"source_id"sv, "capture_method"sv,
                                               "evidence_id"sv};
        if (!audit_object(provenance, provenance_fields) ||
            !read_string(provenance, "source_id", metadata.provenance.source_id) ||
            !read_string(provenance, "capture_method",
                         metadata.provenance.capture_method) ||
            !read_string(provenance, "evidence_id", metadata.provenance.evidence_id))
            return false;
        return metadata.profile_schema_version > 0 &&
               valid_text(metadata.profile_id) &&
               valid_sha256(metadata.profile_digest_sha256) &&
               metadata.source_sample_rate >= 8000.0 &&
               metadata.source_sample_rate <= 384000.0 &&
               metadata.source_frames > 0 && metadata.source_channels > 0 &&
               metadata.source_channels <= kSampleHeritageMaximumChannels &&
               valid_sha256(metadata.source_audio_sha256) &&
               metadata.committed_sample_rate >= 8000.0 &&
               metadata.committed_sample_rate <= 384000.0 &&
               metadata.committed_frames > 0 &&
               metadata.committed_channels > 0 &&
               metadata.committed_channels <= kSampleHeritageMaximumChannels &&
               valid_sha256(metadata.committed_audio_sha256) &&
               valid_provenance(metadata.provenance);
    } catch (...) {
        return false;
    }
}

std::string profile_digest_hex(const SampleHeritagePreparedProfile& profile) {
    return runtime::hex_encode(profile.profile_digest.data(),
                               profile.profile_digest.size());
}

SampleHeritageVoiceReconstructionBlock map_filter(const SampleHeritageRecordRateBlock& source,
    double processing_rate) {
    SampleHeritageVoiceReconstructionBlock result;
    switch (source.filter_family) {
        case SampleHeritageRecordFilterFamily::OnePole:
            result.family = SampleHeritageReconstructionFamily::OnePole;
            break;
        case SampleHeritageRecordFilterFamily::Butterworth:
            result.family = SampleHeritageReconstructionFamily::Butterworth;
            break;
        case SampleHeritageRecordFilterFamily::Chebyshev:
            result.family = SampleHeritageReconstructionFamily::Chebyshev;
            break;
        case SampleHeritageRecordFilterFamily::Elliptic:
            result.family = SampleHeritageReconstructionFamily::Elliptic;
            break;
    }
    result.cutoff_law = SampleHeritageCutoffLaw::FixedHz;
    result.cutoff_value = source.cutoff_law == SampleHeritageCutoffLaw::MachineRateRatio
        ? source.cutoff_value * processing_rate
        : source.cutoff_value;
    result.order = source.order;
    result.ripple_db = source.ripple_db;
    result.stopband_attenuation_db = source.stopband_attenuation_db;
    return result;
}

bool valid_filter_shape(const SampleHeritageVoiceReconstructionBlock& filter,
                        double processing_rate) {
    if (!(filter.cutoff_value > 0.0 && filter.cutoff_value < processing_rate * 0.5))
        return false;
    if (filter.family == SampleHeritageReconstructionFamily::OnePole)
        return filter.order == 1 && filter.ripple_db == 0.0f;
    if (filter.order < 2 || filter.order > 16 || (filter.order & 1u) != 0) return false;
    if (filter.family == SampleHeritageReconstructionFamily::Butterworth)
        return filter.ripple_db == 0.0f;
    return filter.ripple_db > 0.0f;
}

SampleHeritagePreparedProfile dsp_profile(std::string_view id,
    double rate,
    SampleHeritageVoiceBlockParameters parameters) {
    SampleHeritagePreparedProfile profile;
    profile.schema_version = kSampleHeritageProfileSchemaVersion;
    std::copy(id.begin(), id.end(), profile.profile_id.begin());
    profile.host_sample_rate = rate;
    profile.voice_count = 1;
    profile.voice[0].domain = SampleHeritageBlockDomain::Voice;
    profile.voice[0].bypass = false;
    profile.voice[0].parameters = std::move(parameters);
    return profile;
}

bool apply_filter(Buffer<float>& audio,
                  double processing_rate,
                  const SampleHeritageRecordRateBlock& block) {
    auto filter = map_filter(block, processing_rate);
    if (!valid_filter_shape(filter, processing_rate)) return false;
    SampleHeritageVoiceDsp dsp;
    const auto profile = dsp_profile("neutral.record-filter", processing_rate,
                                     std::move(filter));
    if (!dsp.prepare(profile, processing_rate)) return false;
    dsp.process(audio.view());
    return true;
}

bool apply_converter(Buffer<float>& audio,
                     double processing_rate,
                     const SampleHeritageRecordConverterBlock& block) {
    if (block.seed_policy == SampleHeritageSeedPolicy::ContinueSerializedState)
        return false;
    SampleHeritageVoiceConverterBlock converter;
    converter.family = block.family;
    converter.bit_depth = block.bit_depth;
    converter.dac_nonlinearity = block.dac_nonlinearity;
    converter.dither_lsb = block.dither_lsb;
    converter.seed = block.seed;
    converter.seed_policy = block.seed_policy;
    SampleHeritageVoiceDsp dsp;
    const auto profile = dsp_profile("neutral.record-converter", processing_rate,
                                     std::move(converter));
    if (!dsp.prepare(profile, processing_rate)) return false;
    dsp.process(audio.view());
    return true;
}

bool resample_exact(Buffer<float>& audio,
                    double input_rate,
                    double output_rate) {
    if (input_rate == output_rate) return true;
    const auto exact_frames = static_cast<long double>(audio.num_samples()) *
                              static_cast<long double>(output_rate) /
                              static_cast<long double>(input_rate);
    if (!std::isfinite(exact_frames) || exact_frames < 1.0L ||
        exact_frames > static_cast<long double>(std::numeric_limits<std::size_t>::max()) ||
        exact_frames > static_cast<long double>(std::numeric_limits<long long>::max()))
        return false;
    const auto target_frames = static_cast<std::size_t>(std::llround(exact_frames));
    signal::Resampler resampler;
    resampler.prepare(input_rate, output_rate, audio.num_channels(), audio.num_samples());
    const auto padding = resampler.taps_per_phase();
    if (audio.num_samples() > std::numeric_limits<std::size_t>::max() - padding)
        return false;
    Buffer<float> padded(audio.num_channels(), audio.num_samples() + padding);
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
        std::copy(audio.channel(channel).begin(), audio.channel(channel).end(),
                  padded.channel(channel).begin());
    const auto capacity = resampler.max_output_for(padded.num_samples());
    Buffer<float> rendered(audio.num_channels(), capacity);
    std::vector<const float*> inputs(audio.num_channels());
    std::vector<float*> outputs(audio.num_channels());
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel) {
        inputs[channel] = padded.channel(channel).data();
        outputs[channel] = rendered.channel(channel).data();
    }
    const auto produced =
        resampler.process_block(inputs.data(), padded.num_samples(), outputs.data(), capacity);
    const auto delay = static_cast<std::size_t>(
        (static_cast<double>(resampler.taps_per_phase()) - 1.0) * 0.5 *
        output_rate / input_rate);
    if (delay > produced || produced - delay < target_frames) return false;
    Buffer<float> trimmed(audio.num_channels(), target_frames);
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
        std::copy_n(rendered.channel(channel).begin() +
                        static_cast<std::ptrdiff_t>(delay),
                    target_frames, trimmed.channel(channel).begin());
    audio = std::move(trimmed);
    return true;
}

enum class StretchResult { Ok, Invalid, Overflow };

struct StretchZone {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t input_frames = 0;
    std::size_t output_frames = 0;
    std::size_t total_frames = 0;
};

template <typename Block>
StretchResult stretch_zone(const Buffer<float>& audio, const Block& block, StretchZone& zone) {
    const auto whole_asset = block.zone_start_frame == 0 && block.zone_end_frame == 0;
    if (!whole_asset && (block.zone_start_frame > std::numeric_limits<std::size_t>::max() ||
                         block.zone_end_frame > std::numeric_limits<std::size_t>::max()))
        return StretchResult::Invalid;
    const auto end =
        whole_asset ? audio.num_samples() : static_cast<std::size_t>(block.zone_end_frame);
    const auto begin =
        whole_asset ? std::size_t{0} : static_cast<std::size_t>(block.zone_start_frame);
    if (begin >= end || end > audio.num_samples())
        return StretchResult::Invalid;
    const auto input_frames = end - begin;
    const auto exact_output =
        static_cast<long double>(input_frames) * static_cast<long double>(block.factor);
    if (!std::isfinite(exact_output) || exact_output < 1.0L ||
        exact_output > static_cast<long double>(std::numeric_limits<std::size_t>::max()) ||
        exact_output > static_cast<long double>(std::numeric_limits<long long>::max()))
        return StretchResult::Overflow;
    const auto output_frames = static_cast<std::size_t>(std::llround(exact_output));
    const auto suffix_frames = audio.num_samples() - end;
    if (begin > std::numeric_limits<std::size_t>::max() - output_frames ||
        begin + output_frames > std::numeric_limits<std::size_t>::max() - suffix_frames)
        return StretchResult::Overflow;
    zone = {begin, end, input_frames, output_frames, begin + output_frames + suffix_frames};
    return StretchResult::Ok;
}

double fade_in(std::size_t offset, std::size_t length) {
    if (length == 0)
        return 1.0;
    constexpr double half_pi = 1.57079632679489661923;
    const auto phase = (static_cast<double>(offset) + 0.5) / static_cast<double>(length);
    const auto sine = std::sin(half_pi * phase);
    return sine * sine;
}

void add_grain(const Buffer<float>& source, const StretchZone& zone, Buffer<float>& rendered,
               std::size_t channel, std::size_t input_anchor, std::size_t output_anchor,
               std::size_t grain_frames, std::size_t crossfade_frames) {
    const auto first = output_anchor == 0;
    const auto last = output_anchor + grain_frames >= zone.output_frames;
    const auto count = std::min(grain_frames, zone.output_frames - output_anchor);
    for (std::size_t frame = 0; frame < count; ++frame) {
        double weight = 1.0;
        if (!first && frame < crossfade_frames)
            weight = fade_in(frame, crossfade_frames);
        if (!last && frame >= grain_frames - crossfade_frames)
            weight = fade_in(grain_frames - frame - 1, crossfade_frames);
        rendered.channel(channel)[output_anchor + frame] +=
            static_cast<float>(weight) * source.channel(channel)[zone.begin + input_anchor + frame];
    }
}

StretchResult publish_stretched_zone(Buffer<float>& audio, const StretchZone& zone,
                                     Buffer<float>& rendered) {
    Buffer<float> result(audio.num_channels(), zone.total_frames);
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel) {
        std::copy_n(audio.channel(channel).begin(), zone.begin, result.channel(channel).begin());
        std::copy(rendered.channel(channel).begin(), rendered.channel(channel).end(),
                  result.channel(channel).begin() + static_cast<std::ptrdiff_t>(zone.begin));
        std::copy(audio.channel(channel).begin() + static_cast<std::ptrdiff_t>(zone.end),
                  audio.channel(channel).end(),
                  result.channel(channel).begin() +
                      static_cast<std::ptrdiff_t>(zone.begin + zone.output_frames));
    }
    audio = std::move(result);
    return StretchResult::Ok;
}

StretchResult apply_cyclic_stretch(Buffer<float>& audio,
                                   const SampleHeritageRecordCommitCyclicStretchBlock& block) {
    StretchZone zone;
    const auto status = stretch_zone(audio, block, zone);
    if (status != StretchResult::Ok || block.factor == 1.0)
        return status;
    const auto grain_frames = static_cast<std::size_t>(block.cycle_samples);
    const auto crossfade_frames = static_cast<std::size_t>(block.crossfade_samples);
    if (grain_frames > zone.input_frames)
        return StretchResult::Invalid;
    const auto output_hop = grain_frames - crossfade_frames;
    if (output_hop == 0)
        return StretchResult::Invalid;
    Buffer<float> rendered(audio.num_channels(), zone.output_frames);
    const auto maximum_anchor = zone.input_frames - grain_frames;
    for (std::size_t output_anchor = 0; output_anchor < zone.output_frames;) {
        const auto nominal =
            static_cast<long double>(output_anchor) / static_cast<long double>(block.factor);
        const auto input_anchor =
            std::min(maximum_anchor, static_cast<std::size_t>(std::llround(nominal)));
        for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
            add_grain(audio, zone, rendered, channel, input_anchor, output_anchor, grain_frames,
                      crossfade_frames);
        if (output_anchor > std::numeric_limits<std::size_t>::max() - output_hop)
            return StretchResult::Overflow;
        output_anchor += output_hop;
    }
    return publish_stretched_zone(audio, zone, rendered);
}

double removed_dc_ncc(const Buffer<float>& audio, const StretchZone& zone,
                      std::size_t channel_begin, std::size_t channel_end, std::size_t reference,
                      std::size_t candidate, std::size_t frames) {
    if (frames == 0)
        return 0.0;
    long double dot = 0.0L;
    long double reference_energy = 0.0L;
    long double candidate_energy = 0.0L;
    for (std::size_t channel = channel_begin; channel < channel_end; ++channel) {
        long double reference_sum = 0.0L;
        long double candidate_sum = 0.0L;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            reference_sum += audio.channel(channel)[zone.begin + reference + frame];
            candidate_sum += audio.channel(channel)[zone.begin + candidate + frame];
        }
        const auto reference_mean = reference_sum / static_cast<long double>(frames);
        const auto candidate_mean = candidate_sum / static_cast<long double>(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto left =
                static_cast<long double>(audio.channel(channel)[zone.begin + reference + frame]) -
                reference_mean;
            const auto right =
                static_cast<long double>(audio.channel(channel)[zone.begin + candidate + frame]) -
                candidate_mean;
            dot += left * right;
            reference_energy += left * left;
            candidate_energy += right * right;
        }
    }
    if (reference_energy <= std::numeric_limits<long double>::epsilon() ||
        candidate_energy <= std::numeric_limits<long double>::epsilon())
        return 0.0;
    return static_cast<double>(dot / std::sqrt(reference_energy * candidate_energy));
}

std::size_t adaptive_anchor(const Buffer<float>& audio, const StretchZone& zone,
                            std::size_t channel_begin, std::size_t channel_end,
                            std::size_t previous_anchor, std::size_t nominal_anchor,
                            std::size_t grain_frames,
                            const SampleHeritageRecordCommitAdaptiveStretchBlock& block) {
    const auto maximum_anchor = zone.input_frames - grain_frames;
    nominal_anchor = std::min(nominal_anchor, maximum_anchor);
    if (block.crossfade_samples == 0 || block.search_radius_samples == 0)
        return nominal_anchor;
    const auto reference = previous_anchor + block.decision_hop_samples;
    double best_score = -std::numeric_limits<double>::infinity();
    std::int64_t best_delta = 0;
    const auto radius = static_cast<std::int64_t>(block.search_radius_samples);
    const auto stride = static_cast<std::int64_t>(block.search_stride_samples);
    const auto consider = [&](std::int64_t delta, double& score, std::int64_t& selected) {
        const auto signed_candidate = static_cast<std::int64_t>(nominal_anchor) + delta;
        if (signed_candidate < 0 || static_cast<std::uint64_t>(signed_candidate) > maximum_anchor)
            return;
        const auto candidate = static_cast<std::size_t>(signed_candidate);
        const auto candidate_score = removed_dc_ncc(audio, zone, channel_begin, channel_end,
                                                    reference, candidate, block.crossfade_samples);
        if (candidate_score > score ||
            (candidate_score == score &&
             (std::abs(delta) < std::abs(selected) ||
              (std::abs(delta) == std::abs(selected) && delta < selected)))) {
            score = candidate_score;
            selected = delta;
        }
    };
    consider(0, best_score, best_delta);
    for (std::int64_t magnitude = stride; magnitude <= radius; magnitude += stride) {
        consider(-magnitude, best_score, best_delta);
        consider(magnitude, best_score, best_delta);
        if (magnitude > radius - stride)
            break;
    }
    return static_cast<std::size_t>(static_cast<std::int64_t>(nominal_anchor) + best_delta);
}

StretchResult apply_adaptive_stretch(Buffer<float>& audio,
                                     const SampleHeritageRecordCommitAdaptiveStretchBlock& block) {
    StretchZone zone;
    const auto status = stretch_zone(audio, block, zone);
    if (status != StretchResult::Ok || block.factor == 1.0)
        return status;
    const auto output_hop = static_cast<std::size_t>(block.decision_hop_samples);
    if (output_hop > std::numeric_limits<std::size_t>::max() - block.crossfade_samples)
        return StretchResult::Overflow;
    const auto grain_frames = output_hop + block.crossfade_samples;
    if (grain_frames > zone.input_frames)
        return StretchResult::Invalid;
    Buffer<float> rendered(audio.num_channels(), zone.output_frames);
    std::vector<std::size_t> previous_anchor(audio.num_channels(), 0);
    for (std::size_t output_anchor = 0; output_anchor < zone.output_frames;) {
        const auto nominal = std::min(
            zone.input_frames - grain_frames,
            static_cast<std::size_t>(std::llround(static_cast<long double>(output_anchor) /
                                                  static_cast<long double>(block.factor))));
        if (output_anchor == 0) {
            for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
                previous_anchor[channel] = nominal;
        } else if (block.stereo_link) {
            const auto selected =
                adaptive_anchor(audio, zone, 0, audio.num_channels(), previous_anchor.front(),
                                nominal, grain_frames, block);
            std::fill(previous_anchor.begin(), previous_anchor.end(), selected);
        } else {
            for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
                previous_anchor[channel] =
                    adaptive_anchor(audio, zone, channel, channel + 1, previous_anchor[channel],
                                    nominal, grain_frames, block);
        }
        for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
            add_grain(audio, zone, rendered, channel, previous_anchor[channel], output_anchor,
                      grain_frames, block.crossfade_samples);
        if (output_anchor > std::numeric_limits<std::size_t>::max() - output_hop)
            return StretchResult::Overflow;
        output_anchor += output_hop;
    }
    return publish_stretched_zone(audio, zone, rendered);
}

}  // namespace

SampleHeritageRecordCommitResult
commit_sample_heritage_recording(const SampleHeritageProfile& profile,
    BufferView<const float> source,
    double source_sample_rate,
    const SampleHeritageRecordProvenance& provenance) {
    const auto validation = validate_sample_heritage_profile(profile);
    if (!validation.valid())
        return fail(SampleHeritageRecordCommitStatus::InvalidProfile,
                    "profile validation failed");
    if (source.empty() || source.num_channels() > kSampleHeritageMaximumChannels ||
        source.num_channels() > std::numeric_limits<std::uint32_t>::max() ||
        source.num_samples() > std::numeric_limits<std::size_t>::max() /
                                   source.num_channels() ||
        source.num_samples() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
        !(source_sample_rate >= 8000.0 && source_sample_rate <= 384000.0) ||
        !std::isfinite(source_sample_rate) ||
        std::abs(source_sample_rate - profile.host_sample_rate) > 1.0e-9)
        return fail(SampleHeritageRecordCommitStatus::InvalidSource,
                    "source dimensions or sample rate are invalid");
    for (std::size_t channel = 0; channel < source.num_channels(); ++channel)
        if (!std::all_of(source.channel(channel).begin(), source.channel(channel).end(),
                         [](float sample) { return std::isfinite(sample); }))
            return fail(SampleHeritageRecordCommitStatus::InvalidSource,
                        "source contains a non-finite sample");
    if (!valid_provenance(provenance))
        return fail(SampleHeritageRecordCommitStatus::InvalidProvenance,
                    "provenance fields must be non-empty and bounded");

    try {
        Buffer<float> committed;
        copy_audio(source, committed);
        double committed_rate = source_sample_rate;
        for (std::size_t index = 0; index < validation.profile.record_commit_count;
             ++index) {
            const auto& spec = validation.profile.record_commit[index];
            if (spec.bypass) continue;
            if (const auto* drive =
                    std::get_if<SampleHeritageRecordInputDriveClipBlock>(&spec.parameters)) {
                for (std::size_t channel = 0; channel < committed.num_channels(); ++channel)
                    for (auto& sample : committed.channel(channel))
                        sample = std::clamp(sample * drive->drive,
                                            -drive->clip_level, drive->clip_level);
            } else if (const auto* rate =
                           std::get_if<SampleHeritageRecordRateBlock>(&spec.parameters)) {
                if (!apply_filter(committed, committed_rate, *rate))
                    return fail(SampleHeritageRecordCommitStatus::InvalidRecordChain,
                                "record filter is not realizable at the source rate");
                if (!resample_exact(committed, committed_rate, rate->sample_rate))
                    return fail(SampleHeritageRecordCommitStatus::SizeOverflow,
                                "record-rate conversion could not be bounded");
                committed_rate = rate->sample_rate;
            } else if (const auto* converter =
                           std::get_if<SampleHeritageRecordConverterBlock>(&spec.parameters)) {
                if (!apply_converter(committed, committed_rate, *converter))
                    return fail(SampleHeritageRecordCommitStatus::InvalidRecordChain,
                                "record converter state is not self-contained");
            } else if (const auto* stretch =
                           std::get_if<SampleHeritageRecordCommitCyclicStretchBlock>(
                               &spec.parameters)) {
                const auto status = apply_cyclic_stretch(committed, *stretch);
                if (status == StretchResult::Overflow)
                return fail(SampleHeritageRecordCommitStatus::SizeOverflow,
                                "cyclic stretch output size overflowed");
                if (status != StretchResult::Ok)
                    return fail(SampleHeritageRecordCommitStatus::InvalidRecordChain,
                                "cyclic stretch cannot render the selected zone");
            } else if (const auto* stretch =
                           std::get_if<SampleHeritageRecordCommitAdaptiveStretchBlock>(
                               &spec.parameters)) {
                const auto status = apply_adaptive_stretch(committed, *stretch);
                if (status == StretchResult::Overflow)
                    return fail(SampleHeritageRecordCommitStatus::SizeOverflow,
                                "adaptive stretch output size overflowed");
                if (status != StretchResult::Ok)
                    return fail(SampleHeritageRecordCommitStatus::InvalidRecordChain,
                                "adaptive stretch cannot render the selected zone");
            }
        }

        SampleHeritageCommittedAssetMetadata metadata;
        metadata.profile_schema_version = profile.schema_version;
        metadata.profile_id = profile.profile_id;
        metadata.profile_digest_sha256 = profile_digest_hex(validation.profile);
        metadata.source_sample_rate = source_sample_rate;
        metadata.source_frames = source.num_samples();
        metadata.source_channels = static_cast<std::uint32_t>(source.num_channels());
        metadata.source_audio_sha256 = audio_sha256(source, source_sample_rate);
        metadata.committed_sample_rate = committed_rate;
        metadata.committed_frames = committed.num_samples();
        metadata.committed_channels =
            static_cast<std::uint32_t>(committed.num_channels());
        metadata.committed_audio_sha256 =
            audio_sha256(static_cast<const Buffer<float>&>(committed).view(), committed_rate);
        metadata.provenance = provenance;
        auto json = write_metadata(metadata);

        SampleHeritageRecordCommitResult result;
        result.status = SampleHeritageRecordCommitStatus::Ok;
        result.asset.emplace(std::move(committed), std::move(metadata), std::move(json));
        return result;
    } catch (const std::bad_alloc&) {
        return fail(SampleHeritageRecordCommitStatus::AllocationFailed,
                    "record commit allocation failed");
    } catch (...) {
        return fail(SampleHeritageRecordCommitStatus::InvalidRecordChain,
                    "record commit processing failed");
    }
}

SampleHeritageRecordCommitResult reload_sample_heritage_committed_asset(
    const SampleHeritageProfile& profile,
    BufferView<const float> committed_audio,
    double committed_sample_rate,
    std::string_view canonical_metadata_json) {
    const auto validation = validate_sample_heritage_profile(profile);
    if (!validation.valid())
        return fail(SampleHeritageRecordCommitStatus::InvalidProfile,
                    "profile validation failed");
    if (committed_audio.empty() ||
        committed_audio.num_channels() > kSampleHeritageMaximumChannels ||
        !(committed_sample_rate >= 8000.0 && committed_sample_rate <= 384000.0) ||
        !std::isfinite(committed_sample_rate))
        return fail(SampleHeritageRecordCommitStatus::InvalidSource,
                    "committed audio dimensions or sample rate are invalid");
    for (std::size_t channel = 0; channel < committed_audio.num_channels(); ++channel)
        if (!std::all_of(committed_audio.channel(channel).begin(),
                         committed_audio.channel(channel).end(),
                         [](float sample) { return std::isfinite(sample); }))
            return fail(SampleHeritageRecordCommitStatus::InvalidSource,
                        "committed audio contains a non-finite sample");

    try {
        SampleHeritageCommittedAssetMetadata metadata;
        if (!parse_metadata(canonical_metadata_json, metadata))
            return fail(SampleHeritageRecordCommitStatus::InvalidMetadata,
                        "committed asset metadata is invalid");
        if (write_metadata(metadata) != canonical_metadata_json)
            return fail(SampleHeritageRecordCommitStatus::NonCanonicalMetadata,
                        "committed asset metadata is not canonical");
        if (metadata.profile_schema_version != profile.schema_version ||
            metadata.profile_id != profile.profile_id ||
            metadata.profile_digest_sha256 != profile_digest_hex(validation.profile))
            return fail(SampleHeritageRecordCommitStatus::ProfileMismatch,
                        "committed asset profile identity does not match");
        if (metadata.committed_channels != committed_audio.num_channels() ||
            metadata.committed_frames != committed_audio.num_samples() ||
            metadata.committed_sample_rate != committed_sample_rate)
            return fail(SampleHeritageRecordCommitStatus::InvalidMetadata,
                        "committed audio dimensions do not match metadata");
        if (metadata.committed_audio_sha256 !=
            audio_sha256(committed_audio, committed_sample_rate))
            return fail(SampleHeritageRecordCommitStatus::AudioHashMismatch,
                        "committed audio hash does not match metadata");

        Buffer<float> owned;
        copy_audio(committed_audio, owned);
        SampleHeritageRecordCommitResult result;
        result.status = SampleHeritageRecordCommitStatus::Ok;
        result.asset.emplace(std::move(owned), std::move(metadata),
                             std::string(canonical_metadata_json));
        return result;
    } catch (const std::bad_alloc&) {
        return fail(SampleHeritageRecordCommitStatus::AllocationFailed,
                    "committed asset reload allocation failed");
    } catch (...) {
        return fail(SampleHeritageRecordCommitStatus::InvalidMetadata,
                    "committed asset reload failed");
    }
}

}  // namespace pulp::audio
