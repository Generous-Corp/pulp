#include <pulp/audio/audio_reader.hpp>

#include "aiff_parser.hpp"
#include "decode_limits_detail.hpp"

#include "byte_source_cursor.hpp"
#include "wav_frame_reader.hpp"

#include <dr_flac.h>
#include <dr_mp3.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <streambuf>
#include <utility>
#include <vector>

namespace pulp::audio {

namespace {

// Frames decoded per codec call. Bounds every scratch buffer so memory follows
// the request, not the file.
constexpr std::uint64_t kChunkFrames = 4096u;

// ── Byte sources ────────────────────────────────────────────────────────

class MemoryByteSource final : public AudioByteSource {
  public:
    explicit MemoryByteSource(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::size_t read(void* destination, std::size_t bytes) override {
        // seek() refuses positions past the end, so position_ <= size and
        // this subtraction cannot wrap.
        const auto available = bytes_.size() - static_cast<std::size_t>(position_);
        const auto count = std::min(bytes, available);
        if (count > 0)
            std::memcpy(destination, bytes_.data() + position_, count);
        position_ += count;
        return count;
    }
    bool seek(std::uint64_t position) override {
        if (position > bytes_.size())
            return false;
        position_ = position;
        return true;
    }
    std::uint64_t position() const override {
        return position_;
    }
    std::uint64_t size() const override {
        return bytes_.size();
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::uint64_t position_ = 0;
};

class FileByteSource final : public AudioByteSource {
  public:
    bool open(const std::string& path) {
        if (!file_.open(std::filesystem::path(path), std::ios::in | std::ios::binary))
            return false;
        const auto end = file_.pubseekoff(0, std::ios::end, std::ios::in);
        if (end == std::streampos(-1) || file_.pubseekpos(0, std::ios::in) != std::streampos(0))
            return false;
        size_ = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
        return true;
    }

    std::size_t read(void* destination, std::size_t bytes) override {
        // Take the minimum in 64 bits so a 32-bit size_t never truncates the
        // remaining length of a large file.
        const auto wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(bytes, size_ - position_));
        if (wanted == 0)
            return 0;
        const auto got =
            file_.sgetn(static_cast<char*>(destination), static_cast<std::streamsize>(wanted));
        if (got <= 0)
            return 0;
        position_ += static_cast<std::uint64_t>(got);
        return static_cast<std::size_t>(got);
    }
    bool seek(std::uint64_t position) override {
        if (position > size_)
            return false;
        const auto target = static_cast<std::streamoff>(position);
        if (file_.pubseekpos(target, std::ios::in) != std::streampos(target))
            return false;
        position_ = position;
        return true;
    }
    std::uint64_t position() const override {
        return position_;
    }
    std::uint64_t size() const override {
        return size_;
    }

  private:
    std::filebuf file_;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
};

// Buffered, seekable std::streambuf over an AudioByteSource, for the AIFF
// parser, which reads through std::istream.
class SourceStreamBuf final : public std::streambuf {
  public:
    explicit SourceStreamBuf(AudioByteSource& source) : source_(source) {
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }

  protected:
    int_type underflow() override {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        const auto got = source_.read(buffer_.data(), buffer_.size());
        if (got == 0)
            return traits_type::eof();
        setg(buffer_.data(), buffer_.data(), buffer_.data() + got);
        return traits_type::to_int_type(*gptr());
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode which) override {
        if ((which & std::ios_base::in) == 0)
            return pos_type(off_type(-1));
        const auto current = static_cast<std::int64_t>(source_.position()) -
                             static_cast<std::int64_t>(egptr() - gptr());
        std::int64_t base = 0;
        if (direction == std::ios_base::cur)
            base = current;
        else if (direction == std::ios_base::end)
            base = static_cast<std::int64_t>(source_.size());
        const auto target = base + static_cast<std::int64_t>(offset);
        if (target < 0 || static_cast<std::uint64_t>(target) > source_.size())
            return pos_type(off_type(-1));
        if (target == current)
            return pos_type(off_type(target));
        if (!source_.seek(static_cast<std::uint64_t>(target)))
            return pos_type(off_type(-1));
        setg(buffer_.data(), buffer_.data(), buffer_.data());
        return pos_type(off_type(target));
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
        return seekoff(off_type(position), std::ios_base::beg, which);
    }

  private:
    AudioByteSource& source_;
    std::array<char, 65536> buffer_{};
};

// dr_libs callbacks. Both libraries pass the origin as SET, CUR or END in
// that order, so one translation serves both.
std::size_t source_read(void* user, void* destination, std::size_t bytes) {
    return static_cast<AudioByteSource*>(user)->read(destination, bytes);
}

bool source_seek(void* user, int offset, int origin) {
    auto& source = *static_cast<AudioByteSource*>(user);
    std::int64_t base = 0;
    if (origin == 1)
        base = static_cast<std::int64_t>(source.position());
    else if (origin == 2)
        base = static_cast<std::int64_t>(source.size());
    const auto target = base + offset;
    return target >= 0 && source.seek(static_cast<std::uint64_t>(target));
}

static_assert(DRFLAC_SEEK_SET == 0 && DRFLAC_SEEK_CUR == 1 && DRFLAC_SEEK_END == 2);
static_assert(DRMP3_SEEK_SET == 0 && DRMP3_SEEK_CUR == 1 && DRMP3_SEEK_END == 2);

drflac_bool32 flac_seek(void* user, int offset, drflac_seek_origin origin) {
    return source_seek(user, offset, static_cast<int>(origin)) ? DRFLAC_TRUE : DRFLAC_FALSE;
}
drflac_bool32 flac_tell(void* user, drflac_int64* cursor) {
    *cursor = static_cast<drflac_int64>(static_cast<AudioByteSource*>(user)->position());
    return DRFLAC_TRUE;
}
drmp3_bool32 mp3_seek(void* user, int offset, drmp3_seek_origin origin) {
    return source_seek(user, offset, static_cast<int>(origin)) ? DRMP3_TRUE : DRMP3_FALSE;
}
drmp3_bool32 mp3_tell(void* user, drmp3_int64* cursor) {
    *cursor = static_cast<drmp3_int64>(static_cast<AudioByteSource*>(user)->position());
    return DRMP3_TRUE;
}

void deinterleave(const float* interleaved, float* const* out, std::uint64_t frames,
                  std::uint32_t channels) {
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        float* planar = out[ch];
        for (std::uint64_t i = 0; i < frames; ++i)
            planar[i] = interleaved[i * channels + ch];
    }
}

// ── Format detection ────────────────────────────────────────────────────

enum class Container { Unknown, Wav, Aiff, Flac, Mp3, Ogg };

Container container_from_extension(std::string_view hint) {
    std::string ext(hint);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!ext.empty() && ext.front() != '.')
        ext.insert(ext.begin(), '.');
    if (ext == ".wav" || ext == ".wave")
        return Container::Wav;
    if (ext == ".aif" || ext == ".aiff" || ext == ".aifc")
        return Container::Aiff;
    if (ext == ".flac")
        return Container::Flac;
    if (ext == ".mp3")
        return Container::Mp3;
    if (ext == ".ogg" || ext == ".oga")
        return Container::Ogg;
    return Container::Unknown;
}

Container sniff_container(AudioByteSource& source) {
    std::array<std::uint8_t, 12> head{};
    const bool rewound = source.seek(0);
    const auto got = rewound ? source.read(head.data(), head.size()) : 0;
    if (!source.seek(0))
        return Container::Unknown;
    const auto starts_with = [&](std::size_t at, std::string_view tag) {
        return got >= at + tag.size() && std::memcmp(head.data() + at, tag.data(), tag.size()) == 0;
    };
    if ((starts_with(0, "RIFF") || starts_with(0, "RF64")) && starts_with(8, "WAVE"))
        return Container::Wav;
    if (starts_with(0, "FORM") && (starts_with(8, "AIFF") || starts_with(8, "AIFC")))
        return Container::Aiff;
    if (starts_with(0, "fLaC"))
        return Container::Flac;
    if (starts_with(0, "OggS"))
        return Container::Ogg;
    // An ID3v2 tag also prefixes some FLAC files. Such a FLAC with no
    // extension sniffs as MP3, fails to open, and the caller falls back to
    // the registry reader; a ".flac" hint avoids the detour.
    if (starts_with(0, "ID3") || (got >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0))
        return Container::Mp3;
    return Container::Unknown;
}

struct StreamingDecoder;

} // namespace

// ── Decoders ────────────────────────────────────────────────────────────

struct AudioReader::Decoder {
    virtual ~Decoder() = default;

    // Decodes up to `frames` frames starting at `frame_offset` into
    // info.num_channels planar buffers. The caller keeps the range inside
    // [0, info.num_frames). Returns the frames produced.
    virtual std::uint64_t decode(std::uint64_t frame_offset, float* const* out,
                                 std::uint64_t frames) = 0;

    virtual bool ranged() const {
        return true;
    }

    // Non-null for codecs that decode forward from a seek position.
    virtual StreamingDecoder* as_streaming() {
        return nullptr;
    }

    AudioFileInfo info;
    // Planar scratch for reads that ask for fewer channels than the file has.
    std::vector<float> subset_scratch;
    std::vector<float*> subset_pointers;
};

namespace {

using Decoder = AudioReader::Decoder;

void finish_info(AudioFileInfo& info, std::string format) {
    info.format = std::move(format);
    info.duration_seconds =
        info.sample_rate > 0 ? static_cast<double>(info.num_frames) / info.sample_rate : 0.0;
}

// A codec that decodes forward from a seekable position. Sequential reads
// continue from the cursor without seeking, which keeps streaming playback
// from paying a seek per block.
struct StreamingDecoder : Decoder {
    StreamingDecoder* as_streaming() override {
        return this;
    }

    virtual bool seek_to(std::uint64_t frame) = 0;
    // Decodes at most kChunkFrames frames forward; zero at end of stream.
    virtual std::uint64_t decode_next(float* const* out, std::uint64_t frames) = 0;

    std::uint64_t decode(std::uint64_t frame_offset, float* const* out,
                         std::uint64_t frames) override {
        if (!cursor_valid_ || cursor_ != frame_offset) {
            cursor_valid_ = seek_to(frame_offset);
            if (!cursor_valid_)
                return 0;
            cursor_ = frame_offset;
        }
        shifted_.resize(info.num_channels);
        std::uint64_t produced = 0;
        while (produced < frames) {
            for (std::uint32_t ch = 0; ch < info.num_channels; ++ch)
                shifted_[ch] = out[ch] + produced;
            const auto want = std::min(frames - produced, kChunkFrames);
            const auto got = decode_next(shifted_.data(), want);
            if (got == 0)
                break;
            produced += got;
        }
        cursor_ += produced;
        // A short decode means an error or the real end of the stream; seek
        // again next time rather than trusting a cursor the codec has left.
        if (produced < frames)
            cursor_valid_ = false;
        return produced;
    }

    // Decodes the whole stream from the start, for a stream whose header
    // gives no length and so cannot be seeked by frame.
    std::optional<AudioFileData> decode_all(const DecodeLimits& limits) {
        if (!seek_to(0))
            return std::nullopt;
        const auto channels = info.num_channels;
        AudioFileData data;
        data.sample_rate = info.sample_rate;
        data.channels.resize(channels);
        std::vector<float> scratch(static_cast<std::size_t>(kChunkFrames) * channels);
        std::vector<float*> pointers(channels);
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            pointers[ch] = scratch.data() + static_cast<std::size_t>(ch) * kChunkFrames;
        std::uint64_t total = 0;
        for (;;) {
            const auto got = decode_next(pointers.data(), kChunkFrames);
            if (got == 0)
                break;
            if (!detail::within_decode_limits(info.format, total + got, channels, limits))
                return std::nullopt;
            for (std::uint32_t ch = 0; ch < channels; ++ch)
                data.channels[ch].insert(data.channels[ch].end(), pointers[ch], pointers[ch] + got);
            total += got;
        }
        cursor_valid_ = false;
        if (total == 0)
            return std::nullopt;
        return data;
    }

  private:
    std::vector<float*> shifted_;
    std::uint64_t cursor_ = 0;
    bool cursor_valid_ = true;
};

// Serves ranges from a copy decoded once.
struct DecodedDecoder final : Decoder {
    explicit DecodedDecoder(AudioFileData decoded, AudioFileInfo source_info)
        : data(std::move(decoded)) {
        info = std::move(source_info);
        info.sample_rate = data.sample_rate;
        info.num_channels = data.num_channels();
        info.num_frames = data.num_frames();
        finish_info(info, info.format);
    }

    std::uint64_t decode(std::uint64_t frame_offset, float* const* out,
                         std::uint64_t frames) override {
        const auto start = static_cast<std::size_t>(frame_offset);
        const auto count = static_cast<std::size_t>(frames);
        for (std::uint32_t ch = 0; ch < info.num_channels; ++ch)
            std::copy_n(data.channels[ch].data() + start, count, out[ch]);
        return frames;
    }

    bool ranged() const override {
        return false;
    }

    AudioFileData data;
};

struct FlacDecoder final : StreamingDecoder {
    ~FlacDecoder() override {
        if (flac != nullptr)
            drflac_close(flac);
    }

    bool open(std::unique_ptr<AudioByteSource> byte_source) {
        source = std::move(byte_source);
        flac = drflac_open(source_read, flac_seek, flac_tell, source.get(), nullptr);
        if (flac == nullptr || flac->channels == 0 || flac->sampleRate == 0)
            return false;
        info.sample_rate = flac->sampleRate;
        info.num_channels = flac->channels;
        info.num_frames = flac->totalPCMFrameCount;
        info.bits_per_sample = flac->bitsPerSample;
        finish_info(info, "FLAC");
        interleaved.resize(static_cast<std::size_t>(kChunkFrames) * info.num_channels);
        return true;
    }

    bool seek_to(std::uint64_t frame) override {
        return drflac_seek_to_pcm_frame(flac, frame) == DRFLAC_TRUE;
    }

    std::uint64_t decode_next(float* const* out, std::uint64_t frames) override {
        const auto got =
            drflac_read_pcm_frames_f32(flac, std::min(frames, kChunkFrames), interleaved.data());
        deinterleave(interleaved.data(), out, got, info.num_channels);
        return got;
    }

    std::unique_ptr<AudioByteSource> source;
    drflac* flac = nullptr;
    std::vector<float> interleaved;
};

struct Mp3Decoder final : StreamingDecoder {
    ~Mp3Decoder() override {
        if (initialized)
            drmp3_uninit(&mp3);
    }

    bool open(std::unique_ptr<AudioByteSource> byte_source) {
        source = std::move(byte_source);
        initialized = drmp3_init(&mp3, source_read, mp3_seek, mp3_tell, nullptr, source.get(),
                                 nullptr) == DRMP3_TRUE;
        if (!initialized || mp3.channels == 0 || mp3.sampleRate == 0)
            return false;
        // No seek table: the vendored dr_mp3's seek-table path lands whole
        // MP3 frames away from the requested frame, while its decode-forward
        // seek is exact. A forward seek decodes on from the cursor and a
        // backward seek decodes from the start of the stream; neither keeps
        // more than one MP3 frame of output.
        info.sample_rate = mp3.sampleRate;
        info.num_channels = mp3.channels;
        info.num_frames = drmp3_get_pcm_frame_count(&mp3);
        info.bits_per_sample = 16;
        finish_info(info, "MP3");
        interleaved.resize(static_cast<std::size_t>(kChunkFrames) * info.num_channels);
        return drmp3_seek_to_pcm_frame(&mp3, 0) == DRMP3_TRUE;
    }

    bool seek_to(std::uint64_t frame) override {
        // dr_mp3 treats a seek to its own position as done. That position can
        // only equal the target here after a short read, when the decoder may
        // be stuck at a failed frame, so rewind to the start first.
        if (frame == mp3.currentPCMFrame && drmp3_seek_to_pcm_frame(&mp3, 0) != DRMP3_TRUE)
            return false;
        return drmp3_seek_to_pcm_frame(&mp3, frame) == DRMP3_TRUE;
    }

    std::uint64_t decode_next(float* const* out, std::uint64_t frames) override {
        const auto got =
            drmp3_read_pcm_frames_f32(&mp3, std::min(frames, kChunkFrames), interleaved.data());
        deinterleave(interleaved.data(), out, got, info.num_channels);
        return got;
    }

    std::unique_ptr<AudioByteSource> source;
    drmp3 mp3{};
    bool initialized = false;
    std::vector<float> interleaved;
};

struct VorbisDecoder final : StreamingDecoder {
    ~VorbisDecoder() override {
        if (vorbis != nullptr)
            stb_vorbis_close(vorbis);
    }

    bool open_memory(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > static_cast<std::size_t>(INT_MAX))
            return false;
        int error = 0;
        vorbis =
            stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
        return finish_open();
    }

    bool open_file(const std::string& path) {
        int error = 0;
        vorbis = stb_vorbis_open_filename(path.c_str(), &error, nullptr);
        return finish_open();
    }

    bool seek_to(std::uint64_t frame) override {
        // stb_vorbis addresses samples with unsigned int.
        if (frame > std::numeric_limits<unsigned int>::max())
            return false;
        return stb_vorbis_seek(vorbis, static_cast<unsigned int>(frame)) == 1;
    }

    std::uint64_t decode_next(float* const* out, std::uint64_t frames) override {
        const auto got = stb_vorbis_get_samples_float(
            vorbis, static_cast<int>(info.num_channels), const_cast<float**>(out),
            static_cast<int>(std::min(frames, kChunkFrames)));
        return got > 0 ? static_cast<std::uint64_t>(got) : 0u;
    }

    std::vector<std::uint8_t> owned_bytes;
    stb_vorbis* vorbis = nullptr;

  private:
    bool finish_open() {
        if (vorbis == nullptr)
            return false;
        const auto vorbis_info = stb_vorbis_get_info(vorbis);
        if (vorbis_info.channels <= 0 || vorbis_info.sample_rate == 0)
            return false;
        info.sample_rate = vorbis_info.sample_rate;
        info.num_channels = static_cast<std::uint32_t>(vorbis_info.channels);
        info.num_frames = stb_vorbis_stream_length_in_samples(vorbis);
        info.bits_per_sample = 16;
        finish_info(info, "OGG");
        return true;
    }
};

struct WavDecoder final : Decoder {
    bool open(std::unique_ptr<AudioByteSource> byte_source) {
        source = std::move(byte_source);
        cursor = detail::cursor_over(*source);
        const auto probe = detail::probe_wav(cursor);
        if (!probe.accepted() || !reader.open(cursor, probe))
            return false;
        info = detail::wav_file_info(probe);
        return true;
    }

    std::uint64_t decode(std::uint64_t frame_offset, float* const* out,
                         std::uint64_t frames) override {
        const auto channels = info.num_channels;
        interleaved.resize(static_cast<std::size_t>(kChunkFrames) * channels);
        std::uint64_t produced = 0;
        while (produced < frames) {
            const auto count = std::min(frames - produced, kChunkFrames);
            const auto got =
                reader.read_interleaved(frame_offset + produced, count, interleaved.data());
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                float* planar = out[ch] + produced;
                for (std::uint64_t i = 0; i < got; ++i)
                    planar[i] = interleaved[static_cast<std::size_t>(i) * channels + ch];
            }
            produced += got;
            if (got < count)
                break;
        }
        return produced;
    }

    // Declared in dependency order so the reader is destroyed before the
    // cursor's source.
    std::unique_ptr<AudioByteSource> source;
    WaveByteCursor cursor;
    detail::WavFrameReader reader;
    std::vector<float> interleaved;
};

struct AiffDecoder final : Decoder {
    bool open(std::unique_ptr<AudioByteSource> byte_source) {
        source = std::move(byte_source);
        SourceStreamBuf buffer(*source);
        std::istream stream(&buffer);
        auto parsed = detail::parse_aiff_layout(stream, source->size(), true);
        if (!parsed || parsed->info.num_channels == 0 || parsed->bytes_per_frame == 0)
            return false;
        layout = *parsed;
        info = layout.info;
        finish_info(info, info.format.empty() ? "AIFF" : info.format);
        bytes.resize(static_cast<std::size_t>(kChunkFrames) * layout.bytes_per_frame);
        return true;
    }

    std::uint64_t decode(std::uint64_t frame_offset, float* const* out,
                         std::uint64_t frames) override {
        std::uint64_t produced = 0;
        while (produced < frames) {
            const auto count = std::min(frames - produced, kChunkFrames);
            const auto position =
                layout.sample_data_offset + (frame_offset + produced) * layout.bytes_per_frame;
            const auto wanted = static_cast<std::size_t>(count * layout.bytes_per_frame);
            if (!source->seek(position) || source->read(bytes.data(), wanted) != wanted)
                break;
            for (std::uint64_t frame = 0; frame < count; ++frame) {
                const auto* frame_bytes =
                    bytes.data() + static_cast<std::size_t>(frame) * layout.bytes_per_frame;
                for (std::uint32_t ch = 0; ch < info.num_channels; ++ch) {
                    out[ch][produced + frame] = detail::decode_aiff_pcm_sample(
                        frame_bytes + static_cast<std::size_t>(ch) * layout.bytes_per_sample,
                        info.bits_per_sample, layout.little_endian);
                }
            }
            produced += count;
        }
        return produced;
    }

    std::unique_ptr<AudioByteSource> source;
    detail::AiffLayout layout;
    std::vector<std::uint8_t> bytes;
};

template <typename T>
std::unique_ptr<Decoder> open_with_source(std::unique_ptr<AudioByteSource> source) {
    auto decoder = std::make_unique<T>();
    if (!decoder->open(std::move(source)))
        return nullptr;
    return decoder;
}

// Reads every byte of `source` into memory, refusing more than `max_bytes`
// (zero means no ceiling beyond what stb_vorbis can address).
std::optional<std::vector<std::uint8_t>> read_all_bytes(AudioByteSource& source,
                                                        std::uint64_t max_bytes) {
    const auto size = source.size();
    if ((max_bytes != 0 && size > max_bytes) || size > static_cast<std::uint64_t>(INT_MAX) ||
        !source.seek(0))
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (source.read(bytes.data(), bytes.size()) != bytes.size())
        return std::nullopt;
    return bytes;
}

std::unique_ptr<Decoder> open_seekable(std::unique_ptr<AudioByteSource> source, Container container,
                                       const DecodeLimits& limits,
                                       std::span<const std::uint8_t> memory) {
    switch (container) {
    case Container::Wav:
        return open_with_source<WavDecoder>(std::move(source));
    case Container::Aiff:
        return open_with_source<AiffDecoder>(std::move(source));
    case Container::Flac:
        return open_with_source<FlacDecoder>(std::move(source));
    case Container::Mp3:
        return open_with_source<Mp3Decoder>(std::move(source));
    case Container::Ogg: {
        auto decoder = std::make_unique<VorbisDecoder>();
        if (memory.empty()) {
            auto bytes = read_all_bytes(*source, limits.max_output_bytes);
            if (!bytes)
                return nullptr;
            decoder->owned_bytes = std::move(*bytes);
            memory = decoder->owned_bytes;
        }
        if (!decoder->open_memory(memory))
            return nullptr;
        return decoder;
    }
    case Container::Unknown:
        break;
    }
    return nullptr;
}

// Applies the decode limits to a freshly opened decoder. The header's frame
// and channel claims are checked before any read. A ranged decoder never holds
// the whole stream, so max_output_bytes bounds each allocating read rather
// than the file's length. A stream with no length claim is decoded once, with
// every ceiling checked as it decodes, so its frames can be addressed.
std::unique_ptr<Decoder> admit(std::unique_ptr<Decoder> decoder, const DecodeLimits& limits,
                               AudioReaderFallback fallback) {
    if (!decoder)
        return nullptr;
    const auto& info = decoder->info;
    auto claim_limits = limits;
    claim_limits.max_output_bytes = 0;
    if (!detail::within_decode_limits(info.format, info.num_frames, info.num_channels,
                                      claim_limits))
        return nullptr;
    // A seekable container's frame count is exact, so zero frames is a valid
    // empty file (the WAV and AIFF decoders report it as the registry does).
    // Only a streaming codec's zero means "no length claim".
    if (info.num_frames != 0 || decoder->as_streaming() == nullptr)
        return decoder;
    auto* streaming = decoder->as_streaming();
    if (streaming == nullptr || fallback == AudioReaderFallback::Refuse)
        return nullptr;
    auto decoded = streaming->decode_all(limits);
    if (!decoded)
        return nullptr;
    return std::make_unique<DecodedDecoder>(std::move(*decoded), info);
}

std::unique_ptr<Decoder> open_decoded_file(const std::string& path, const DecodeLimits& limits) {
    auto& registry = FormatRegistry::instance();
    auto data = registry.read(path, limits);
    if (!data || data->empty())
        return nullptr;
    AudioFileInfo info;
    if (auto header = registry.read_info(path)) {
        info.bits_per_sample = header->bits_per_sample;
        info.format = header->format;
    }
    return std::make_unique<DecodedDecoder>(std::move(*data), std::move(info));
}

} // namespace

// ── Byte source factories ───────────────────────────────────────────────

std::unique_ptr<AudioByteSource> make_memory_byte_source(std::span<const std::uint8_t> bytes) {
    return std::make_unique<MemoryByteSource>(bytes);
}

std::unique_ptr<AudioByteSource> make_file_byte_source(const std::string& path) {
    auto source = std::make_unique<FileByteSource>();
    if (!source->open(path))
        return nullptr;
    return source;
}

// ── AudioReader ─────────────────────────────────────────────────────────

AudioReader::AudioReader(std::unique_ptr<Decoder> decoder, const DecodeLimits& limits)
    : decoder_(std::move(decoder)), limits_(limits) {}

AudioReader::~AudioReader() = default;

std::unique_ptr<AudioReader> AudioReader::open_file(const std::string& path,
                                                    const DecodeLimits& limits,
                                                    AudioReaderFallback fallback) {
    auto container = container_from_extension(std::filesystem::path(path).extension().string());
    std::unique_ptr<Decoder> decoder;
    if (container == Container::Ogg) {
        // stb_vorbis seeks a FILE directly, so the file need not be in memory.
        auto vorbis = std::make_unique<VorbisDecoder>();
        if (vorbis->open_file(path))
            decoder = std::move(vorbis);
    } else {
        auto source = make_file_byte_source(path);
        if (!source)
            return nullptr;
        if (container == Container::Unknown)
            container = sniff_container(*source);
        decoder = open_seekable(std::move(source), container, limits, {});
    }
    if (decoder) {
        decoder = admit(std::move(decoder), limits, fallback);
        if (!decoder)
            return nullptr;
    } else if (fallback == AudioReaderFallback::Refuse) {
        return nullptr;
    } else {
        // No seekable decoder (for example a compressed AIFF-C or a format
        // the platform reader decodes); the registry reader enforces limits.
        decoder = open_decoded_file(path, limits);
        if (!decoder)
            return nullptr;
    }
    return std::unique_ptr<AudioReader>(new AudioReader(std::move(decoder), limits));
}

std::unique_ptr<AudioReader> AudioReader::open_memory(std::span<const std::uint8_t> bytes,
                                                      std::string_view format_hint,
                                                      const DecodeLimits& limits,
                                                      AudioReaderFallback fallback) {
    if (bytes.empty())
        return nullptr;
    auto source = make_memory_byte_source(bytes);
    auto container = container_from_extension(format_hint);
    if (container == Container::Unknown)
        container = sniff_container(*source);
    auto decoder =
        admit(open_seekable(std::move(source), container, limits, bytes), limits, fallback);
    if (!decoder)
        return nullptr;
    return std::unique_ptr<AudioReader>(new AudioReader(std::move(decoder), limits));
}

std::unique_ptr<AudioReader> AudioReader::open_source(std::unique_ptr<AudioByteSource> source,
                                                      std::string_view format_hint,
                                                      const DecodeLimits& limits,
                                                      AudioReaderFallback fallback) {
    if (!source || source->size() == 0)
        return nullptr;
    auto container = container_from_extension(format_hint);
    if (container == Container::Unknown)
        container = sniff_container(*source);
    if (!source->seek(0))
        return nullptr;
    auto decoder = admit(open_seekable(std::move(source), container, limits, {}), limits, fallback);
    if (!decoder)
        return nullptr;
    return std::unique_ptr<AudioReader>(new AudioReader(std::move(decoder), limits));
}

const AudioFileInfo& AudioReader::info() const {
    return decoder_->info;
}

bool AudioReader::supports_ranged_read() const {
    return decoder_->ranged();
}

std::uint64_t AudioReader::read(std::uint64_t frame_offset, float* const* destinations,
                                std::uint32_t channels, std::uint64_t frames) {
    if (destinations == nullptr || channels == 0 || frames == 0)
        return 0;
    auto& decoder = *decoder_;
    const auto file_channels = decoder.info.num_channels;
    const auto total = decoder.info.num_frames;
    const auto wanted = frame_offset < total ? std::min(frames, total - frame_offset) : 0u;

    std::uint64_t produced = 0;
    if (wanted > 0 && channels >= file_channels) {
        produced = decoder.decode(frame_offset, destinations, wanted);
    } else if (wanted > 0) {
        // The codec writes every file channel; decode into scratch and keep
        // the requested prefix.
        decoder.subset_scratch.resize(static_cast<std::size_t>(kChunkFrames) * file_channels);
        decoder.subset_pointers.resize(file_channels);
        for (std::uint32_t ch = 0; ch < file_channels; ++ch)
            decoder.subset_pointers[ch] =
                decoder.subset_scratch.data() + static_cast<std::size_t>(ch) * kChunkFrames;
        while (produced < wanted) {
            const auto count = std::min(wanted - produced, kChunkFrames);
            const auto got =
                decoder.decode(frame_offset + produced, decoder.subset_pointers.data(), count);
            for (std::uint32_t ch = 0; ch < channels; ++ch)
                std::copy_n(decoder.subset_pointers[ch], got, destinations[ch] + produced);
            produced += got;
            if (got < count)
                break;
        }
    }

    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        const auto filled = ch < file_channels ? produced : 0u;
        std::fill(destinations[ch] + filled, destinations[ch] + frames, 0.0f);
    }
    return produced;
}

std::optional<AudioFileData> AudioReader::read(std::uint64_t frame_offset, std::uint64_t frames) {
    const auto& info = decoder_->info;
    const auto total = info.num_frames;
    const auto count = frame_offset < total ? std::min(frames, total - frame_offset) : 0u;
    if (!detail::within_decode_limits(info.format, count, info.num_channels, limits_))
        return std::nullopt;
    AudioFileData data;
    data.sample_rate = info.sample_rate;
    data.channels.assign(info.num_channels, std::vector<float>(static_cast<std::size_t>(count)));
    std::vector<float*> pointers(info.num_channels);
    for (std::uint32_t ch = 0; ch < info.num_channels; ++ch)
        pointers[ch] = data.channels[ch].data();
    const auto produced = read(frame_offset, pointers.data(), info.num_channels, count);
    for (auto& channel : data.channels)
        channel.resize(static_cast<std::size_t>(produced));
    return data;
}

} // namespace pulp::audio
