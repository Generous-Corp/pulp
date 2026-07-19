// AIFF reader/writer — Audio Interchange File Format
// Supports AIFF and AIFF-C (compressed) with 8/16/24/32-bit PCM.
// Critical for Logic Pro projects and sample libraries.

#include <pulp/audio/format_registry.hpp>
#include "aiff_parser.hpp"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <limits>

namespace pulp::audio {

// ── AIFF chunk parsing ──────────────────────────────────────────────────

static uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint16_t read_be16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;  p[3] = v & 0xFF;
}

static void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}

// Convert 80-bit IEEE 754 extended to double (used for AIFF sample rate)
static double extended_to_double(const uint8_t* p) {
    int sign = (p[0] >> 7) & 1;
    int exponent = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
        mantissa = (mantissa << 8) | p[2 + i];

    if (exponent == 0 && mantissa == 0) return 0.0;

    double val = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return sign ? -val : val;
}

namespace detail {

std::optional<AiffLayout> parse_aiff_layout(std::istream& stream,
                                            uint64_t file_size,
                                            bool require_pcm_data) {
    if (file_size < 12 ||
        file_size > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }

    stream.clear();
    stream.seekg(0, std::ios::beg);
    uint8_t header[12];
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::memcmp(header, "FORM", 4) != 0) {
        return std::nullopt;
    }

    const bool is_aifc = std::memcmp(header + 8, "AIFC", 4) == 0;
    if (!is_aifc && std::memcmp(header + 8, "AIFF", 4) != 0)
        return std::nullopt;

    const uint64_t form_size = read_be32(header + 4);
    if (form_size < 4 || form_size > file_size - 8)
        return std::nullopt;
    const uint64_t form_end = 8 + form_size;

    AiffLayout layout;
    layout.info.format = is_aifc ? "AIFF-C" : "AIFF";
    bool has_comm = false;
    bool pcm_supported = !is_aifc;
    bool has_sound = false;
    bool invalid_sound_offset = false;

    uint64_t chunk_offset = 12;
    while (chunk_offset <= form_end - 8) {
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(chunk_offset), std::ios::beg);
        uint8_t chunk_header[8];
        stream.read(reinterpret_cast<char*>(chunk_header), sizeof(chunk_header));
        if (stream.gcount() != static_cast<std::streamsize>(sizeof(chunk_header)))
            return std::nullopt;

        const uint64_t chunk_size = read_be32(chunk_header + 4);
        const uint64_t data_offset = chunk_offset + 8;
        if (chunk_size > form_end - data_offset)
            return std::nullopt;
        const uint64_t chunk_end = data_offset + chunk_size;
        if ((chunk_size & 1u) != 0 && chunk_end == form_end)
            return std::nullopt;
        const uint64_t padded_end = chunk_end + (chunk_size & 1u);
        if (padded_end > form_end)
            return std::nullopt;

        if (std::memcmp(chunk_header, "COMM", 4) == 0) {
            if (chunk_size < 18) return std::nullopt;
            uint8_t comm[22] = {};
            const auto bytes_to_read = static_cast<std::streamsize>(
                std::min<uint64_t>(chunk_size, sizeof(comm)));
            stream.read(reinterpret_cast<char*>(comm), bytes_to_read);
            if (stream.gcount() != bytes_to_read) return std::nullopt;

            layout.info.num_channels = read_be16(comm);
            layout.info.num_frames = read_be32(comm + 2);
            layout.info.bits_per_sample = read_be16(comm + 6);
            const double sample_rate = extended_to_double(comm + 8);
            if (!std::isfinite(sample_rate) || sample_rate <= 0.0 ||
                sample_rate > std::numeric_limits<uint32_t>::max() ||
                layout.info.num_channels == 0) {
                return std::nullopt;
            }
            layout.info.sample_rate = static_cast<uint32_t>(sample_rate);
            layout.info.duration_seconds = static_cast<double>(layout.info.num_frames)
                                         / layout.info.sample_rate;
            pcm_supported = !is_aifc ||
                (chunk_size >= 22 && std::memcmp(comm + 18, "NONE", 4) == 0);
            has_comm = true;
        } else if (std::memcmp(chunk_header, "SSND", 4) == 0) {
            if (chunk_size >= 8) {
                uint8_t sound_header[8];
                stream.read(reinterpret_cast<char*>(sound_header), sizeof(sound_header));
                if (stream.gcount() != static_cast<std::streamsize>(sizeof(sound_header)))
                    return std::nullopt;
                const uint64_t offset = read_be32(sound_header);
                const uint64_t payload_size = chunk_size - 8;
                if (offset > payload_size) {
                    invalid_sound_offset = true;
                } else {
                    layout.sample_data_offset = data_offset + 8 + offset;
                    layout.sample_data_size = payload_size - offset;
                    has_sound = true;
                }
            }
        }

        chunk_offset = padded_end;
    }

    if (chunk_offset != form_end || !has_comm) return std::nullopt;
    if (!require_pcm_data) return layout;
    if (!pcm_supported || !has_sound || invalid_sound_offset ||
        layout.info.num_frames == 0 ||
        (layout.info.bits_per_sample != 8 && layout.info.bits_per_sample != 16 &&
         layout.info.bits_per_sample != 24 && layout.info.bits_per_sample != 32)) {
        return std::nullopt;
    }

    layout.bytes_per_sample = layout.info.bits_per_sample / 8;
    const uint64_t bytes_per_frame =
        static_cast<uint64_t>(layout.info.num_channels) * layout.bytes_per_sample;
    if (bytes_per_frame > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    layout.bytes_per_frame = static_cast<uint32_t>(bytes_per_frame);
    if (layout.info.num_frames > layout.sample_data_size / bytes_per_frame)
        return std::nullopt;
    return layout;
}

float decode_aiff_pcm_sample(const uint8_t* bytes, uint32_t bits_per_sample) {
    if (bits_per_sample == 8)
        return static_cast<float>(static_cast<int8_t>(bytes[0])) / 128.0f;
    if (bits_per_sample == 16) {
        const auto value = static_cast<int16_t>(
            (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
        return static_cast<float>(value) / 32768.0f;
    }
    if (bits_per_sample == 24) {
        const uint32_t raw = (static_cast<uint32_t>(bytes[0]) << 24) |
                             (static_cast<uint32_t>(bytes[1]) << 16) |
                             (static_cast<uint32_t>(bytes[2]) << 8);
        return static_cast<float>(static_cast<int32_t>(raw)) / 2147483648.0f;
    }
    return static_cast<float>(static_cast<int32_t>(read_be32(bytes))) / 2147483648.0f;
}

}  // namespace detail

// Convert double to 80-bit IEEE 754 extended
static void double_to_extended(double val, uint8_t* p) {
    std::memset(p, 0, 10);
    if (val == 0.0) return;

    int sign = val < 0 ? 1 : 0;
    if (sign) val = -val;

    int exponent;
    double mantissa = std::frexp(val, &exponent);
    exponent += 16383 - 1;

    uint64_t m = static_cast<uint64_t>(mantissa * std::pow(2.0, 64));

    p[0] = static_cast<uint8_t>((sign << 7) | ((exponent >> 8) & 0x7F));
    p[1] = static_cast<uint8_t>(exponent & 0xFF);
    for (int i = 0; i < 8; ++i)
        p[2 + i] = static_cast<uint8_t>((m >> (56 - i * 8)) & 0xFF);
}

static bool has_consistent_channel_lengths(const AudioFileData& data) {
    if (data.empty()) return false;

    const auto expected_frames = data.num_frames();
    return std::all_of(data.channels.begin(), data.channels.end(),
                       [expected_frames](const auto& channel) {
                           return channel.size() == expected_frames;
                       });
}

static bool can_write_aiff_pcm16(const AudioFileData& data) {
    if (data.sample_rate == 0 || !has_consistent_channel_lengths(data))
        return false;

    const uint32_t channels = data.num_channels();
    const uint64_t frames = data.num_frames();
    if (channels == 0 || channels > std::numeric_limits<uint16_t>::max() ||
        frames > std::numeric_limits<uint32_t>::max())
        return false;

    constexpr uint32_t bytes_per_sample = 2;
    const uint64_t bytes_per_frame = static_cast<uint64_t>(channels) * bytes_per_sample;
    return frames <=
        (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 8u) / bytes_per_frame;
}

// ── AIFF Reader ─────────────────────────────────────────────────────────

class AiffReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        std::error_code error;
        const auto file_size = std::filesystem::file_size(path, error);
        if (error) return std::nullopt;
        auto layout = detail::parse_aiff_layout(file, file_size, false);
        if (!layout) return std::nullopt;
        return layout->info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;

        std::error_code error;
        const auto file_size = std::filesystem::file_size(path, error);
        if (error) return std::nullopt;
        auto layout = detail::parse_aiff_layout(file, file_size, true);
        if (!layout) return std::nullopt;

        AudioFileData data;
        data.sample_rate = layout->info.sample_rate;
        data.channels.resize(layout->info.num_channels);
        for (auto& ch : data.channels)
            ch.resize(layout->info.num_frames);

        std::vector<uint8_t> frame(layout->bytes_per_frame);
        file.clear();
        file.seekg(static_cast<std::streamoff>(layout->sample_data_offset), std::ios::beg);
        for (uint32_t f = 0; f < layout->info.num_frames; ++f) {
            file.read(reinterpret_cast<char*>(frame.data()),
                      static_cast<std::streamsize>(frame.size()));
            if (file.gcount() != static_cast<std::streamsize>(frame.size()))
                return std::nullopt;
            for (uint32_t c = 0; c < layout->info.num_channels; ++c) {
                data.channels[c][f] = detail::decode_aiff_pcm_sample(
                    frame.data() + static_cast<size_t>(c) * layout->bytes_per_sample,
                    layout->info.bits_per_sample);
            }
        }

        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".aiff" || ext == ".aif" || ext == ".aifc";
    }
    std::string format_name() const override { return "AIFF"; }
};

// ── AIFF Writer ─────────────────────────────────────────────────────────

class AiffWriter : public FormatWriter {
public:
    bool write(const std::string& path, const AudioFileData& data) override {
        if (!can_write_aiff_pcm16(data)) return false;

        uint32_t num_channels = data.num_channels();
        uint64_t frames = data.num_frames();
        if (num_channels > std::numeric_limits<uint16_t>::max()
            || frames > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        for (const auto& channel : data.channels)
            if (channel.size() != static_cast<size_t>(frames))
                return false;

        uint32_t num_frames = static_cast<uint32_t>(frames);
        uint16_t bits = 16;
        int bytes_per_sample = 2;

        uint64_t ssnd_data_bytes = static_cast<uint64_t>(num_frames)
                                 * static_cast<uint64_t>(num_channels)
                                 * static_cast<uint64_t>(bytes_per_sample);
        if (ssnd_data_bytes > std::numeric_limits<uint32_t>::max() - 8u)
            return false;

        uint32_t ssnd_data_size = static_cast<uint32_t>(ssnd_data_bytes);
        uint32_t ssnd_chunk_size = ssnd_data_size + 8;  // +8 for offset and blockSize
        uint32_t comm_chunk_size = 18;
        uint32_t form_size = 4 + 8 + comm_chunk_size + 8 + ssnd_chunk_size;

        std::ofstream file(path, std::ios::binary);
        if (!file) return false;

        // FORM header
        uint8_t form[12];
        std::memcpy(form, "FORM", 4);
        write_be32(form + 4, form_size);
        std::memcpy(form + 8, "AIFF", 4);
        file.write(reinterpret_cast<char*>(form), 12);

        // COMM chunk
        uint8_t comm[26];
        std::memcpy(comm, "COMM", 4);
        write_be32(comm + 4, comm_chunk_size);
        write_be16(comm + 8, static_cast<uint16_t>(num_channels));
        write_be32(comm + 10, num_frames);
        write_be16(comm + 14, bits);
        double_to_extended(static_cast<double>(data.sample_rate), comm + 16);
        file.write(reinterpret_cast<char*>(comm), 26);

        // SSND chunk
        uint8_t ssnd_header[16];
        std::memcpy(ssnd_header, "SSND", 4);
        write_be32(ssnd_header + 4, ssnd_chunk_size);
        write_be32(ssnd_header + 8, 0);  // offset
        write_be32(ssnd_header + 12, 0);  // blockSize
        file.write(reinterpret_cast<char*>(ssnd_header), 16);

        // Interleaved sample data (big-endian 16-bit)
        for (uint32_t f = 0; f < num_frames; ++f) {
            for (uint32_t c = 0; c < num_channels; ++c) {
                float sample = std::clamp(data.channels[c][f], -1.0f, 1.0f);
                int16_t v = static_cast<int16_t>(sample * 32767.0f);
                uint8_t bytes[2] = {static_cast<uint8_t>((v >> 8) & 0xFF),
                                     static_cast<uint8_t>(v & 0xFF)};
                file.write(reinterpret_cast<char*>(bytes), 2);
            }
        }

        return file.good();
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".aiff" || ext == ".aif";
    }
    std::string format_name() const override { return "AIFF"; }
};

std::unique_ptr<FormatReader> create_aiff_reader() {
    return std::make_unique<AiffReader>();
}

std::unique_ptr<FormatWriter> create_aiff_writer() {
    return std::make_unique<AiffWriter>();
}

}  // namespace pulp::audio
