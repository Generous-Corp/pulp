// AIFF reader/writer — Audio Interchange File Format
// Supports AIFF and AIFF-C (compressed) with 8/16/24/32-bit PCM.
// Critical for Logic Pro projects and sample libraries.

#include <pulp/audio/format_registry.hpp>
#include <fstream>
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

        uint8_t header[12];
        file.read(reinterpret_cast<char*>(header), 12);
        if (file.gcount() != 12) return std::nullopt;

        if (std::memcmp(header, "FORM", 4) != 0) return std::nullopt;
        bool is_aifc = (std::memcmp(header + 8, "AIFC", 4) == 0);
        if (!is_aifc && std::memcmp(header + 8, "AIFF", 4) != 0) return std::nullopt;

        AudioFileInfo info;
        info.format = is_aifc ? "AIFF-C" : "AIFF";

        while (file.good()) {
            uint8_t chunk_header[8];
            file.read(reinterpret_cast<char*>(chunk_header), 8);
            if (file.gcount() != 8) break;

            uint32_t chunk_size = read_be32(chunk_header + 4);

            if (std::memcmp(chunk_header, "COMM", 4) == 0) {
                if (chunk_size < 18) return std::nullopt;

                uint8_t comm[18] = {};
                file.read(reinterpret_cast<char*>(comm), 18);
                if (file.gcount() != 18) return std::nullopt;

                info.num_channels = read_be16(comm);
                info.num_frames = read_be32(comm + 2);
                info.bits_per_sample = read_be16(comm + 6);
                info.sample_rate = static_cast<uint32_t>(extended_to_double(comm + 8));
                if (info.sample_rate == 0 || info.num_channels == 0)
                    return std::nullopt;
                info.duration_seconds = static_cast<double>(info.num_frames) / info.sample_rate;
                return info;
            }

            // Skip chunk (pad to even)
            file.seekg(chunk_size + (chunk_size & 1), std::ios::cur);
        }

        return std::nullopt;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;

        uint8_t header[12];
        file.read(reinterpret_cast<char*>(header), 12);
        if (file.gcount() != 12) return std::nullopt;
        if (std::memcmp(header, "FORM", 4) != 0) return std::nullopt;
        if (std::memcmp(header + 8, "AIFF", 4) != 0 &&
            std::memcmp(header + 8, "AIFC", 4) != 0) return std::nullopt;

        uint32_t num_channels = 0, num_frames = 0, bits_per_sample = 0;
        double sample_rate = 0;
        std::vector<uint8_t> ssnd_data;

        while (file.good()) {
            uint8_t chunk_header[8];
            file.read(reinterpret_cast<char*>(chunk_header), 8);
            if (file.gcount() != 8) break;

            uint32_t chunk_size = read_be32(chunk_header + 4);

            if (std::memcmp(chunk_header, "COMM", 4) == 0) {
                if (chunk_size < 18) return std::nullopt;

                uint32_t bytes_to_read = std::min(chunk_size, 26u);
                uint8_t comm[26] = {};
                file.read(reinterpret_cast<char*>(comm), bytes_to_read);
                if (file.gcount() != static_cast<std::streamsize>(bytes_to_read))
                    return std::nullopt;
                num_channels = read_be16(comm);
                num_frames = read_be32(comm + 2);
                bits_per_sample = read_be16(comm + 6);
                sample_rate = extended_to_double(comm + 8);
                // Skip remaining bytes in chunk (common in AIFC with compression type)
                uint32_t remaining = chunk_size - bytes_to_read;
                if (remaining > 0)
                    file.seekg(remaining, std::ios::cur);
                // Pad to even boundary
                if (chunk_size & 1) file.seekg(1, std::ios::cur);
            } else if (std::memcmp(chunk_header, "SSND", 4) == 0) {
                if (chunk_size < 8) {
                    file.seekg(chunk_size + (chunk_size & 1), std::ios::cur);
                    continue;  // Malformed SSND — skip
                }
                uint8_t ssnd_header[8];
                file.read(reinterpret_cast<char*>(ssnd_header), 8);
                if (file.gcount() != 8) return std::nullopt;
                uint32_t offset = read_be32(ssnd_header);
                size_t data_size = chunk_size - 8;
                ssnd_data.resize(data_size);
                file.read(reinterpret_cast<char*>(ssnd_data.data()), static_cast<std::streamsize>(data_size));
                if (file.gcount() != static_cast<std::streamsize>(data_size))
                    return std::nullopt;
                if (offset > ssnd_data.size())
                    return std::nullopt;
                if (offset > 0)
                    ssnd_data.erase(ssnd_data.begin(), ssnd_data.begin() + static_cast<std::ptrdiff_t>(offset));
                if (chunk_size & 1) file.seekg(1, std::ios::cur);
            } else {
                file.seekg(chunk_size + (chunk_size & 1), std::ios::cur);
            }
        }

        if (num_channels == 0 || num_frames == 0 || sample_rate <= 0.0 || ssnd_data.empty())
            return std::nullopt;

        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(sample_rate);
        data.channels.resize(num_channels);
        for (auto& ch : data.channels)
            ch.resize(num_frames);

        int bytes_per_sample = static_cast<int>(bits_per_sample) / 8;
        int frame_size = bytes_per_sample * static_cast<int>(num_channels);
        if ((bits_per_sample != 8 && bits_per_sample != 16 &&
             bits_per_sample != 24 && bits_per_sample != 32) ||
            ssnd_data.size() < static_cast<size_t>(frame_size) * num_frames) {
            return std::nullopt;
        }

        for (uint32_t f = 0; f < num_frames; ++f) {
            for (uint32_t c = 0; c < num_channels; ++c) {
                size_t offset = static_cast<size_t>(f) * frame_size + static_cast<size_t>(c) * bytes_per_sample;
                if (offset + bytes_per_sample > ssnd_data.size()) break;

                const uint8_t* p = ssnd_data.data() + offset;
                float sample = 0;

                if (bits_per_sample == 16) {
                    int16_t v = static_cast<int16_t>((p[0] << 8) | p[1]);
                    sample = static_cast<float>(v) / 32768.0f;
                } else if (bits_per_sample == 24) {
                    int32_t v = (static_cast<int32_t>(p[0]) << 24) |
                                (static_cast<int32_t>(p[1]) << 16) |
                                (static_cast<int32_t>(p[2]) << 8);
                    sample = static_cast<float>(v) / 2147483648.0f;
                } else if (bits_per_sample == 32) {
                    int32_t v = static_cast<int32_t>(read_be32(p));
                    sample = static_cast<float>(v) / 2147483648.0f;
                } else if (bits_per_sample == 8) {
                    // AIFF 8-bit PCM is signed (-128 to 127)
                    int8_t v = static_cast<int8_t>(p[0]);
                    sample = static_cast<float>(v) / 128.0f;
                }

                data.channels[c][f] = sample;
            }
        }

        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".aiff" || ext == ".aif";
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
