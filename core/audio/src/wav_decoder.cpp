#include <pulp/audio/wav_decoder.hpp>

#include "wav_frame_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#define DRWAV_API [[maybe_unused]] static
#define DRWAV_PRIVATE static
#define DR_WAV_LIBSNDFILE_COMPAT
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

namespace pulp::audio {
namespace {

// Interleaved samples decoded per dr_wav call: bounds the scratch buffer.
constexpr std::size_t kInterleavedChunkSamples = 65'536u;
constexpr std::uint16_t kTagExtensible = 0xFFFEu;

std::uint16_t u16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8u));
}

// The stream dr_wav reads: a canonical header, then `data_bytes` of the
// file's data payload starting at `data_offset`. Nothing else of the file
// is ever presented, so dr_wav's chunk walk sees one fmt and one data chunk.
struct CanonicalStream {
    CanonicalWaveHeader header;
    WaveByteCursor source;
    std::uint64_t data_offset = 0u;
    std::uint64_t data_bytes = 0u;
    std::uint64_t position = 0u;

    std::uint64_t size() const noexcept {
        return header.size + data_bytes;
    }
};

std::size_t read_stream(void* user, void* output, std::size_t requested) noexcept {
    auto& stream = *static_cast<CanonicalStream*>(user);
    auto* out = static_cast<std::uint8_t*>(output);
    std::size_t copied = 0u;
    while (copied < requested && stream.position < stream.size()) {
        const auto wanted = requested - copied;
        std::size_t got = 0u;
        if (stream.position < stream.header.size) {
            got = std::min<std::size_t>(wanted, stream.header.size -
                                                    static_cast<std::size_t>(stream.position));
            std::memcpy(out + copied, stream.header.bytes.data() + stream.position, got);
        } else {
            const auto into_data = stream.position - stream.header.size;
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(wanted, stream.data_bytes - into_data));
            got = stream.source.read(stream.data_offset + into_data, out + copied, count);
            if (got == 0u)
                break;
        }
        copied += got;
        stream.position += got;
    }
    return copied;
}

drwav_bool32 seek_stream(void* user, int offset, drwav_seek_origin origin) noexcept {
    auto& stream = *static_cast<CanonicalStream*>(user);
    std::int64_t base = 0;
    if (origin == DRWAV_SEEK_CUR)
        base = static_cast<std::int64_t>(stream.position);
    else if (origin == DRWAV_SEEK_END)
        base = static_cast<std::int64_t>(stream.size());
    const auto target = base + offset;
    if (target < 0 || static_cast<std::uint64_t>(target) > stream.size())
        return DRWAV_FALSE;
    stream.position = static_cast<std::uint64_t>(target);
    return DRWAV_TRUE;
}

// The formats whose every frame dr_wav decodes. It would open others and
// then read them as silence or not at all, so they are refused up front.
bool decoder_reads(drwav& wav) noexcept {
    const auto tag = wav.translatedFormatTag;
    const auto channels = static_cast<std::uint32_t>(wav.channels);
    if (channels == 0u)
        return true; // reported as zero channels, a refusal of its own
    if (tag == DR_WAVE_FORMAT_ADPCM || tag == DR_WAVE_FORMAT_DVI_ADPCM)
        return channels <= 2u;
    const auto frame = drwav_get_bytes_per_pcm_frame(&wav);
    if (frame == 0u || frame % channels != 0u)
        return false;
    const auto sample = frame / channels;
    if (tag == DR_WAVE_FORMAT_PCM)
        return sample >= 1u && sample <= 4u;
    if (tag == DR_WAVE_FORMAT_IEEE_FLOAT)
        return sample == 4u || sample == 8u;
    if (tag == DR_WAVE_FORMAT_ALAW || tag == DR_WAVE_FORMAT_MULAW)
        return sample == 1u;
    return false;
}

bool is_adpcm(std::uint16_t translated) noexcept {
    return translated == DR_WAVE_FORMAT_ADPCM || translated == DR_WAVE_FORMAT_DVI_ADPCM;
}

class DrWavSession {
  public:
    explicit DrWavSession(CanonicalStream& stream) noexcept
        : initialized_(drwav_init(&wav_, read_stream, seek_stream, nullptr, &stream, nullptr) !=
                       DRWAV_FALSE) {}
    ~DrWavSession() {
        if (initialized_)
            drwav_uninit(&wav_);
    }
    DrWavSession(const DrWavSession&) = delete;
    DrWavSession& operator=(const DrWavSession&) = delete;

    explicit operator bool() const noexcept {
        return initialized_;
    }
    drwav& wav() noexcept {
        return wav_;
    }

  private:
    drwav wav_{};
    bool initialized_ = false;
};

bool checked_output_bytes(std::uint64_t frames, std::uint32_t channels,
                          std::uint64_t& bytes) noexcept {
    constexpr auto kFloatBytes = static_cast<std::uint64_t>(sizeof(float));
    if (channels == 0u || frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (samples > std::numeric_limits<std::uint64_t>::max() / kFloatBytes)
        return false;
    bytes = samples * kFloatBytes;
    return true;
}

} // namespace

// ── Probe and frame reader ──────────────────────────────────────────────────

namespace detail {

WavProbe probe_wav(const WaveByteCursor& cursor) {
    WavProbe probe;
    probe.layout = walk_wave_layout(cursor);
    // The fmt chunk alone decides the format; the frame counts come from the
    // data chunk through decide_wave, so no data size is passed here.
    if (probe.layout.refusal == WaveRefusal::none)
        probe.facts = interpret_wav_format(probe.layout.fmt.bytes(), 0u);
    probe.decision = decide_wave(probe.layout, probe.facts);
    return probe;
}

AudioFileInfo wav_file_info(const WavProbe& probe) {
    AudioFileInfo info;
    if (!probe.facts)
        return info;
    info.sample_rate = probe.facts->sample_rate;
    info.num_channels = probe.facts->channels;
    info.num_frames = probe.decision.recoverable_frames;
    info.bits_per_sample = probe.facts->bits_per_sample;
    info.format = "WAV";
    info.duration_seconds = info.sample_rate != 0u ? static_cast<double>(info.num_frames) /
                                                         static_cast<double>(info.sample_rate)
                                                   : 0.0;
    info.truncated = probe.decision.truncated;
    info.declared_frames = probe.decision.declared_frames;
    return info;
}

bool wav_admits(const WavProbe& probe, const WavDecodeLimits& limits) {
    return probe.accepted() && probe.facts && wav_decode_admits(wav_file_info(probe), limits);
}

struct WavFrameReader::Impl {
    CanonicalStream stream;
    std::optional<DrWavSession> session; // opened once `stream` is filled in
    std::uint64_t frames = 0u;
    std::uint64_t next_frame = 0u;
    std::vector<float> interleaved;
};

WavFrameReader::WavFrameReader() = default;
WavFrameReader::~WavFrameReader() = default;

bool WavFrameReader::open(const WaveByteCursor& cursor, const WavProbe& probe) {
    impl_.reset();
    if (!probe.accepted() || !probe.facts)
        return false;
    auto impl = std::make_unique<Impl>();
    impl->stream.header =
        canonical_wave_header(probe.layout, probe.decision.recoverable_data_bytes);
    impl->stream.source = cursor;
    impl->stream.data_offset = probe.layout.data_offset;
    impl->stream.data_bytes = probe.decision.recoverable_data_bytes;
    impl->session.emplace(impl->stream);
    // The decoder must count exactly the frames the policy decided on; a
    // disagreement is a defect in one of them, refused rather than guessed.
    if (!*impl->session || impl->session->wav().channels != probe.facts->channels ||
        impl->session->wav().totalPCMFrameCount != probe.decision.recoverable_frames)
        return false;
    impl->frames = probe.decision.recoverable_frames;
    impl_ = std::move(impl);
    return true;
}

std::uint32_t WavFrameReader::channels() const noexcept {
    return impl_ ? impl_->session->wav().channels : 0u;
}

std::uint64_t WavFrameReader::frames() const noexcept {
    return impl_ ? impl_->frames : 0u;
}

std::uint64_t WavFrameReader::read_planar(std::uint64_t frame, std::uint64_t count,
                                          float* const* out) {
    if (!impl_ || frame >= impl_->frames || count == 0u)
        return 0u;
    auto& impl = *impl_;
    auto& wav = impl.session->wav();
    if (frame != impl.next_frame) {
        if (drwav_seek_to_pcm_frame(&wav, frame) == DRWAV_FALSE)
            return 0u;
        impl.next_frame = frame;
    }
    const std::size_t channels = wav.channels;
    const std::uint64_t chunk = std::max<std::size_t>(1u, kInterleavedChunkSamples / channels);
    impl.interleaved.resize(static_cast<std::size_t>(chunk) * channels);
    const auto wanted = std::min(count, impl.frames - frame);
    std::uint64_t produced = 0u;
    while (produced < wanted) {
        const auto request = std::min(chunk, wanted - produced);
        const auto got = drwav_read_pcm_frames_f32(&wav, request, impl.interleaved.data());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            float* planar = out[ch] + produced;
            for (std::size_t i = 0; i < static_cast<std::size_t>(got); ++i)
                planar[i] = impl.interleaved[i * channels + ch];
        }
        produced += got;
        impl.next_frame += got;
        if (got < request)
            break;
    }
    return produced;
}

std::optional<AudioFileData> decode_all(const WaveByteCursor& cursor, const WavProbe& probe) {
    WavFrameReader reader;
    if (!reader.open(cursor, probe) || reader.frames() > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    const auto frames = static_cast<std::size_t>(reader.frames());
    AudioFileData decoded;
    decoded.sample_rate = probe.facts->sample_rate;
    decoded.channels.resize(reader.channels());
    std::vector<float*> pointers;
    for (auto& channel : decoded.channels) {
        channel.resize(frames);
        pointers.push_back(channel.data());
    }
    // A zero-frame file is a valid file with no audio: its channels stay
    // empty (`AudioFileData::empty()` is true), the answer libsndfile gives.
    if (frames != 0u && reader.read_planar(0u, frames, pointers.data()) != frames)
        return std::nullopt;
    return decoded;
}

} // namespace detail

// ── Public surface ──────────────────────────────────────────────────────────

std::optional<AudioFileInfo> inspect_wav(std::span<const std::uint8_t> bytes) {
    const auto probe = detail::probe_wav(WaveByteCursor::over(bytes));
    if (!probe.accepted())
        return std::nullopt;
    return detail::wav_file_info(probe);
}

bool wav_decode_admits(const AudioFileInfo& info, const WavDecodeLimits& limits) noexcept {
    if (limits.refuse_truncated && info.truncated)
        return false;
    if (limits.max_frames == 0u || limits.max_channels == 0u || limits.max_output_bytes == 0u)
        return false;
    std::uint64_t output_bytes = 0u;
    return info.num_frames <= limits.max_frames && info.num_channels <= limits.max_channels &&
           checked_output_bytes(info.num_frames, info.num_channels, output_bytes) &&
           output_bytes <= limits.max_output_bytes &&
           output_bytes <= std::numeric_limits<std::size_t>::max();
}

std::optional<WavFormatFacts> interpret_wav_format(std::span<const std::uint8_t> fmt_payload,
                                                   std::uint64_t data_size) {
    if (fmt_payload.size() < 16u)
        return std::nullopt;
    const auto tag = u16(fmt_payload.data());
    if (tag == kTagExtensible && fmt_payload.size() < 40u)
        return std::nullopt; // no sub-format to translate
    const auto translated = tag == kTagExtensible ? u16(fmt_payload.data() + 24u) : tag;
    // dr_wav divides an ADPCM data size by the block align while it opens.
    if (is_adpcm(translated) && u16(fmt_payload.data() + 12u) == 0u)
        return std::nullopt;

    // The header alone: dr_wav stops at the data chunk header, so the
    // payload it declares need not be present.
    CanonicalStream stream;
    stream.header = canonical_wave_header(fmt_payload, data_size);
    DrWavSession session(stream);
    if (!session)
        return std::nullopt;
    auto& wav = session.wav();
    if (!decoder_reads(wav))
        return std::nullopt;

    WavFormatFacts facts;
    facts.format_tag = wav.fmt.formatTag;
    facts.translated_format_tag = wav.translatedFormatTag;
    facts.channels = wav.channels;
    facts.sample_rate = wav.sampleRate;
    facts.bits_per_sample = wav.bitsPerSample;
    facts.bytes_per_frame =
        is_adpcm(wav.translatedFormatTag) ? 0u : drwav_get_bytes_per_pcm_frame(&wav);
    facts.total_frames = wav.totalPCMFrameCount;
    return facts;
}

std::optional<WavFormatFacts> interpret_wav_format(std::span<const std::uint8_t> fmt_payload,
                                                   std::uint64_t data_size,
                                                   std::optional<std::uint64_t>) {
    return interpret_wav_format(fmt_payload, data_size);
}

std::optional<AudioFileData> decode_wav(std::span<const std::uint8_t> bytes,
                                        WavDecodeLimits limits) {
    const auto cursor = WaveByteCursor::over(bytes);
    const auto probe = detail::probe_wav(cursor);
    if (!detail::wav_admits(probe, limits))
        return std::nullopt;
    return detail::decode_all(cursor, probe);
}

} // namespace pulp::audio
