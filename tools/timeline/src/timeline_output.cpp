#include "timeline_agent_internal.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/detail/durable_file_replacement.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <ostream>
#include <streambuf>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pulp::tools::timeline::detail {
namespace {

class DescriptorStreamBuffer final : public std::streambuf {
  public:
    explicit DescriptorStreamBuffer(int descriptor) : descriptor_(descriptor) {}

  protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0)
            return 0;
        std::streamsize offset = 0;
        while (offset < count) {
            const auto remaining = static_cast<std::uint64_t>(count - offset);
#ifdef _WIN32
            const auto chunk = static_cast<unsigned int>(
                std::min<std::uint64_t>(remaining, std::numeric_limits<unsigned int>::max()));
            const auto written = ::_write(descriptor_, data + offset, chunk);
#else
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining, static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())));
            const auto written = ::write(descriptor_, data + offset, chunk);
#endif
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            offset += static_cast<std::streamsize>(written);
        }
        return offset;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode mode) override {
        if ((mode & std::ios_base::out) == 0)
            return pos_type(off_type(-1));
        int whence = SEEK_SET;
        if (direction == std::ios_base::cur)
            whence = SEEK_CUR;
        else if (direction == std::ios_base::end)
            whence = SEEK_END;
#ifdef _WIN32
        const auto position = ::_lseeki64(descriptor_, static_cast<__int64>(offset), whence);
#else
        const auto native_offset = static_cast<off_t>(offset);
        if (static_cast<off_type>(native_offset) != offset)
            return pos_type(off_type(-1));
        const auto position = ::lseek(descriptor_, native_offset, whence);
#endif
        return position < 0 ? pos_type(off_type(-1)) : pos_type(static_cast<off_type>(position));
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode);
    }

  private:
    int descriptor_ = -1;
};

} // namespace

AtomicWriteOutcome write_wav_atomic(const std::filesystem::path& destination,
                                    const pulp::audio::AudioFileData& audio) noexcept {
    auto replacement = pulp::runtime::detail::DurableFileReplacement::create(destination);
    if (!replacement)
        return AtomicWriteOutcome::NotReplaced;
    DescriptorStreamBuffer buffer(replacement->native_descriptor());
    std::ostream output(&buffer);
    if (!pulp::audio::write_wav_stream(output, audio, pulp::audio::WavBitDepth::Float32)) {
        replacement->cancel();
        return AtomicWriteOutcome::NotReplaced;
    }
    switch (replacement->commit()) {
    case pulp::runtime::detail::DurableFileCommitOutcome::NotReplaced:
        return AtomicWriteOutcome::NotReplaced;
    case pulp::runtime::detail::DurableFileCommitOutcome::ReplacedDurably:
        return AtomicWriteOutcome::ReplacedDurably;
    case pulp::runtime::detail::DurableFileCommitOutcome::ReplacedButDirectorySyncFailed:
        return AtomicWriteOutcome::ReplacedButDirectorySyncFailed;
    }
    return AtomicWriteOutcome::NotReplaced;
}

} // namespace pulp::tools::timeline::detail
