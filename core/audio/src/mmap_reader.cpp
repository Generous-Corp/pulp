#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/format_registry.hpp>
#include "aiff_parser.hpp"

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>

#include <algorithm>
#include <istream>
#include <memory>
#include <streambuf>
#include <vector>

namespace pulp::audio {

namespace {

// Read-only, seekable streambuf over a fixed memory block (the mapped file).
// choc's audio reader seeks the stream to a frame offset, so seek support is
// required; no copying of the underlying bytes happens here.
class MappedStreamBuf : public std::streambuf {
public:
    MappedStreamBuf(const char* base, std::size_t size) {
        char* p = const_cast<char*>(base);
        setg(p, p, p + size);
    }

protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which) override {
        if ((which & std::ios_base::in) == 0) return pos_type(off_type(-1));
        char* beg = eback();
        char* end = egptr();
        char* target = nullptr;
        if (dir == std::ios_base::beg) target = beg + off;
        else if (dir == std::ios_base::cur) target = gptr() + off;
        else target = end + off;  // std::ios_base::end
        if (target < beg || target > end) return pos_type(off_type(-1));
        setg(beg, target, end);
        return pos_type(target - beg);
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }
};

}  // namespace

// Ranged decoder state over the mapped bytes. WAV uses a persistent choc reader;
// uncompressed AIFF uses its parsed byte layout directly. Other formats fall
// back to a one-time whole-file decode cached in `fallback`.
struct MemoryMappedAudioReader::RangedState {
    MappedStreamBuf buf;
    std::shared_ptr<std::istream> stream;
    std::unique_ptr<choc::audio::AudioFileReader> reader;
    std::optional<detail::AiffLayout> aiff;
    std::optional<AudioFileData> fallback;  // lazy whole-file decode (non-ranged path)
    // Planar scratch for channel-subset ranged reads (choc requires the view's
    // channel count to match the file exactly, so a mono-from-stereo read decodes
    // all file channels here then copies the requested prefix). Reused per call.
    std::vector<float> subset_scratch;
    std::vector<float*> subset_ptrs;

    RangedState(const char* base, std::size_t size) : buf(base, size) {
        stream = std::make_shared<std::istream>(&buf);
    }
};

// Special members defined here, where RangedState is a complete type.
MemoryMappedAudioReader::MemoryMappedAudioReader() = default;
MemoryMappedAudioReader::~MemoryMappedAudioReader() = default;
MemoryMappedAudioReader::MemoryMappedAudioReader(MemoryMappedAudioReader&&) noexcept = default;
MemoryMappedAudioReader& MemoryMappedAudioReader::operator=(MemoryMappedAudioReader&&) noexcept = default;

bool MemoryMappedAudioReader::open(std::string_view path) {
    close();
    path_ = std::string(path);

    if (!mmap_.open(path))
        return false;

    auto file_info = FormatRegistry::instance().read_info(path_);
    if (!file_info) {
        close();
        return false;
    }
    info_ = *file_info;

    // Build a ranged decoder over the mapped bytes. WAV delegates requested
    // frames to choc; uncompressed AIFF records the validated PCM byte layout.
    // Other formats fall back to a one-time whole-file decode.
    if (mmap_.data() != nullptr && mmap_.size() > 0) {
        ranged_ = std::make_unique<RangedState>(
            reinterpret_cast<const char*>(mmap_.data()), mmap_.size());
        choc::audio::AudioFileFormatList formats;
        formats.addFormat<choc::audio::WAVAudioFileFormat<false>>();
        ranged_->reader = formats.createReader(ranged_->stream);
        if (!ranged_->reader) {
            ranged_->aiff = detail::parse_aiff_layout(
                *ranged_->stream, mmap_.size(), true);
        }
    }
    return true;
}

void MemoryMappedAudioReader::close() {
    ranged_.reset();  // release the reader/stream before unmapping the bytes
    mmap_.close();
    info_ = {};
    path_.clear();
}

bool MemoryMappedAudioReader::supports_ranged_read() const {
    return ranged_ && (ranged_->reader != nullptr || ranged_->aiff.has_value());
}

bool MemoryMappedAudioReader::read_frames(float** dest_channels, uint32_t num_channels,
                                          uint64_t start_frame, uint64_t num_frames) {
    if (!is_open()) return false;
    if (num_channels == 0 || num_frames == 0) return true;
    if (dest_channels == nullptr) return false;

    const uint32_t ch_count = std::min(num_channels, info_.num_channels);
    for (uint32_t c = 0; c < ch_count; ++c)
        if (dest_channels[c] == nullptr) return false;

    const uint64_t total = info_.num_frames;
    const uint64_t start = std::min(start_frame, total);
    const uint64_t count = std::min(num_frames, total - start);
    // Zero-fill any tail past end-of-file so callers always get num_frames.
    for (uint32_t c = 0; c < ch_count; ++c)
        if (count < num_frames)
            std::fill(dest_channels[c] + count, dest_channels[c] + num_frames, 0.0f);
    if (count == 0) return true;

    // Ranged path: decode only [start, start+count) from the mapped bytes.
    if (supports_ranged_read()) {
        if (ranged_->aiff) {
            const auto& layout = *ranged_->aiff;
            const auto* sample_data = mmap_.data() + layout.sample_data_offset;
            for (uint64_t frame = 0; frame < count; ++frame) {
                const auto* source_frame = sample_data
                    + static_cast<size_t>(start + frame) * layout.bytes_per_frame;
                for (uint32_t channel = 0; channel < ch_count; ++channel) {
                    dest_channels[channel][frame] = detail::decode_aiff_pcm_sample(
                        source_frame + static_cast<size_t>(channel) * layout.bytes_per_sample,
                        layout.info.bits_per_sample);
                }
            }
            return true;
        }

        const auto fcount = static_cast<choc::buffer::FrameCount>(count);
        if (ch_count == info_.num_channels) {
            // Exact channel match: decode straight into the caller's buffers.
            auto view = choc::buffer::createChannelArrayView<float>(
                dest_channels, ch_count, fcount);
            if (ranged_->reader->readFrames(start, view))
                return true;
        } else {
            // Channel subset (e.g. mono from a stereo file): choc requires the
            // view's channel count to equal the file's, so decode all file
            // channels into scratch then copy the requested prefix. Still a true
            // ranged decode — no whole-file read, so supports_ranged_read() stays
            // honest for sub-channel callers (a normal sample-library operation).
            auto& rs = *ranged_;
            const std::size_t fch = info_.num_channels;
            rs.subset_scratch.assign(fch * static_cast<std::size_t>(count), 0.0f);
            rs.subset_ptrs.resize(fch);
            for (std::size_t c = 0; c < fch; ++c)
                rs.subset_ptrs[c] =
                    rs.subset_scratch.data() + c * static_cast<std::size_t>(count);
            auto view = choc::buffer::createChannelArrayView<float>(
                rs.subset_ptrs.data(), info_.num_channels, fcount);
            if (rs.reader->readFrames(start, view)) {
                for (uint32_t c = 0; c < ch_count; ++c)
                    std::copy(rs.subset_ptrs[c],
                              rs.subset_ptrs[c] + count, dest_channels[c]);
                return true;
            }
        }
        // A seek/read failure falls through to the whole-file decode below.
    }

    // Fallback: decode the whole file once, cache it, and serve ranges from it.
    if (!ranged_) {
        ranged_ = std::make_unique<RangedState>(
            reinterpret_cast<const char*>(mmap_.data()), mmap_.size());
    }
    if (!ranged_->fallback) {
        ranged_->fallback = FormatRegistry::instance().read(path_);
        if (!ranged_->fallback) return false;
    }
    const auto& data = *ranged_->fallback;
    const uint32_t fb_ch = std::min(ch_count, data.num_channels());
    const uint64_t fb_avail = start < data.num_frames() ? data.num_frames() - start : 0;
    const uint64_t fb_count = std::min(count, fb_avail);
    for (uint32_t c = 0; c < ch_count; ++c) {
        const uint64_t copy = (c < fb_ch) ? fb_count : 0;
        for (uint64_t f = 0; f < copy; ++f)
            dest_channels[c][f] = data.channels[c][static_cast<std::size_t>(start + f)];
        // Zero whatever the fallback couldn't supply within [0, count). A header
        // that over-reports the frame count (MP3 frame estimates are a common
        // case) decodes fewer frames than `count`, and a file with fewer channels
        // than requested supplies none for the surplus — without this the caller's
        // buffer would be returned partly uninitialized as audio.
        if (copy < count)
            std::fill(dest_channels[c] + copy, dest_channels[c] + count, 0.0f);
    }
    return true;
}

std::optional<AudioFileData> MemoryMappedAudioReader::read_all() {
    if (!is_open()) return std::nullopt;
    return FormatRegistry::instance().read(path_);
}

}  // namespace pulp::audio
