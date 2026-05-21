#include <pulp/audio/streaming_writer.hpp>
#include <cstring>
#include <algorithm>
#include <limits>

namespace pulp::audio {

// WAV header writing helpers
static void write_le16(std::ofstream& f, uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
    f.write(reinterpret_cast<char*>(b), 2);
}

static void write_le32(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                     static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
    f.write(reinterpret_cast<char*>(b), 4);
}

StreamingWriter::~StreamingWriter() {
    close();
}

bool StreamingWriter::open(std::string_view path, uint32_t sample_rate,
                            uint32_t num_channels, uint32_t bits_per_sample) {
    close();

    // Only support 16, 24, and 32-bit PCM
    if (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32)
        return false;
    if (num_channels == 0 || sample_rate == 0)
        return false;
    const uint32_t bytes_per_sample = bits_per_sample / 8;
    if (num_channels > std::numeric_limits<uint16_t>::max() ||
        num_channels > std::numeric_limits<uint16_t>::max() / bytes_per_sample)
        return false;

    uint16_t block_align = static_cast<uint16_t>(bytes_per_sample * num_channels);
    if (sample_rate > std::numeric_limits<uint32_t>::max() / block_align)
        return false;

    sample_rate_ = sample_rate;
    num_channels_ = num_channels;
    bits_per_sample_ = bits_per_sample;
    frames_written_ = 0;

    std::string file_path(path);
    file_.open(file_path, std::ios::binary);
    if (!file_) return false;

    // Write WAV header with placeholder sizes (updated in close())
    uint32_t byte_rate = sample_rate * block_align;

    file_.write("RIFF", 4);
    write_le32(file_, 0);  // Placeholder: file size - 8
    file_.write("WAVE", 4);

    // fmt chunk
    file_.write("fmt ", 4);
    write_le32(file_, 16);  // Chunk size
    write_le16(file_, 1);   // PCM format
    write_le16(file_, static_cast<uint16_t>(num_channels));
    write_le32(file_, sample_rate);
    write_le32(file_, byte_rate);
    write_le16(file_, block_align);
    write_le16(file_, static_cast<uint16_t>(bits_per_sample));

    // data chunk header
    file_.write("data", 4);
    write_le32(file_, 0);  // Placeholder: data size
    data_start_pos_ = static_cast<size_t>(file_.tellp());

    return true;
}

int StreamingWriter::write_frames(const float* interleaved_data, int num_frames) {
    if (!file_.is_open() || interleaved_data == nullptr || num_frames <= 0) return 0;
    if (num_channels_ > static_cast<uint32_t>(std::numeric_limits<int>::max() / num_frames))
        return 0;

    int total_samples = num_frames * static_cast<int>(num_channels_);

    for (int i = 0; i < total_samples; ++i) {
        float sample = std::clamp(interleaved_data[i], -1.0f, 1.0f);

        if (bits_per_sample_ == 16) {
            int16_t v = static_cast<int16_t>(sample * 32767.0f);
            uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
            file_.write(reinterpret_cast<char*>(b), 2);
        } else if (bits_per_sample_ == 24) {
            int32_t v = static_cast<int32_t>(sample * 8388607.0f);
            uint8_t b[3] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                             static_cast<uint8_t>((v >> 16) & 0xFF)};
            file_.write(reinterpret_cast<char*>(b), 3);
        } else if (bits_per_sample_ == 32) {
            int32_t v = static_cast<int32_t>(sample * 2147483647.0f);
            uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                             static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
            file_.write(reinterpret_cast<char*>(b), 4);
        }
    }

    frames_written_ += static_cast<uint64_t>(num_frames);
    return num_frames;
}

int StreamingWriter::write_frames(const float* const* channels, int num_channels, int num_frames) {
    if (!file_.is_open() || channels == nullptr
        || num_channels != static_cast<int>(num_channels_) || num_frames <= 0) return 0;
    if (num_channels > std::numeric_limits<int>::max() / num_frames) return 0;
    for (int c = 0; c < num_channels; ++c)
        if (channels[c] == nullptr) return 0;

    // Interleave and write
    std::vector<float> interleaved(static_cast<size_t>(num_frames * num_channels));
    for (int f = 0; f < num_frames; ++f)
        for (int c = 0; c < num_channels; ++c)
            interleaved[static_cast<size_t>(f * num_channels + c)] = channels[c][f];
    return write_frames(interleaved.data(), num_frames);
}

void StreamingWriter::close() {
    if (!file_.is_open()) return;

    // Go back and fix the WAV header sizes
    uint32_t data_size = static_cast<uint32_t>(
        frames_written_ * num_channels_ * (bits_per_sample_ / 8));
    uint32_t riff_size = data_size + 36;  // 36 = header bytes before data

    // Fix RIFF size (offset 4)
    file_.seekp(4);
    write_le32(file_, riff_size);

    // Fix data chunk size (offset data_start_pos_ - 4)
    file_.seekp(static_cast<std::streamoff>(data_start_pos_) - 4);
    write_le32(file_, data_size);

    file_.close();
}

}  // namespace pulp::audio
