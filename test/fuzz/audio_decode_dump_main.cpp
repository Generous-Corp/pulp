// Developer-lane dump: decode one file through the FormatRegistry and write a
// JSON header line followed by raw little-endian float32 interleaved samples
// to stdout. The quality lab's decoder differential compares this stream
// against libsndfile bit for bit; it is never a required gate.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored
//
// Exit codes: 0 decoded, 1 usage, 2 the registry refused the file.

#include <pulp/audio/format_registry.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// Byte-swaps on a big-endian host so the stream is always little-endian.
void put_float_le(float value, std::vector<unsigned char>& out) {
    static_assert(sizeof(float) == 4);
    unsigned char bytes[4];
    std::memcpy(bytes, &value, 4);
    if constexpr (std::endian::native == std::endian::big) {
        std::swap(bytes[0], bytes[3]);
        std::swap(bytes[1], bytes[2]);
    }
    out.insert(out.end(), bytes, bytes + 4);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: pulp-audio-decode-dump <audio file>\n");
        return 1;
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    const std::string path = argv[1];
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto data = registry.read(path, pulp::audio::DecodeLimits{});
    if (!data || data->channels.empty()) {
        std::fprintf(stderr, "pulp-audio-decode-dump: no registered reader decoded %s\n",
                     path.c_str());
        return 2;
    }

    const auto channels = data->num_channels();
    const auto frames = data->num_frames();
    std::printf("{\"schema\":\"pulp.audio.decode_dump.v1\",\"sample_rate\":%u,"
                "\"channels\":%u,\"frames\":%llu}\n",
                static_cast<unsigned>(data->sample_rate), static_cast<unsigned>(channels),
                static_cast<unsigned long long>(frames));
    std::fflush(stdout);

    std::vector<unsigned char> out;
    out.reserve(static_cast<std::size_t>(frames) * channels * sizeof(float));
    for (std::uint64_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            put_float_le(data->channels[c][static_cast<std::size_t>(f)], out);
    if (!out.empty() && std::fwrite(out.data(), 1, out.size(), stdout) != out.size()) {
        std::fprintf(stderr, "pulp-audio-decode-dump: short write to stdout\n");
        return 1;
    }
    std::fflush(stdout);
    return 0;
}
