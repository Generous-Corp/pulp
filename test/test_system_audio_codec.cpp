// System codec writer: files written through the operating system's codecs
// read back with the expected layout and signal. On macOS this covers AAC and
// ALAC in M4A, AAC in ADTS, ALAC and float PCM in CAF and 24-bit AIFF. On
// Windows it covers AAC in M4A read back through Media Foundation. Elsewhere
// every write must be refused.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/system_audio_codec.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::audio;

namespace {

AudioFileData stereo_tone(std::uint32_t rate, std::uint32_t frames) {
    AudioFileData data;
    data.sample_rate = rate;
    data.channels.assign(2, std::vector<float>(frames));
    for (std::uint32_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / rate;
        data.channels[0][f] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
        data.channels[1][f] =
            static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * 660.0 * t));
    }
    return data;
}

struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& extension) {
        // CTest runs each case as its own process, in parallel, so a name
        // must be unique across processes: a per-process counter alone gave
        // two cases the same file, and one removed the other's.
        static const auto process_tag =
            std::to_string(std::random_device{}()) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("pulp_system_codec_" + process_tag + "_" +
                                            std::to_string(counter.fetch_add(1)) + extension);
        fs::remove(path);
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

double rms(const std::vector<float>& samples, std::size_t begin, std::size_t end) {
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += static_cast<double>(samples[i]) * samples[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

// Best normalized correlation of `decoded` against `source` over a middle
// window, searching a small lag range for codec delay.
double best_correlation(const std::vector<float>& source, const std::vector<float>& decoded) {
    const std::size_t begin = source.size() / 4;
    const std::size_t length = source.size() / 2;
    double best = -1.0;
    for (std::size_t lag = 0; lag <= 2112 && begin + lag + length <= decoded.size(); ++lag) {
        double dot = 0.0, a = 0.0, b = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            const double x = source[begin + i];
            const double y = decoded[begin + lag + i];
            dot += x * y;
            a += x * x;
            b += y * y;
        }
        if (a > 0.0 && b > 0.0)
            best = std::max(best, dot / std::sqrt(a * b));
    }
    return best;
}

void require_lossy_match(const AudioFileData& source, const AudioFileData& decoded,
                         std::int64_t max_length_error = 1024) {
    REQUIRE(decoded.sample_rate == source.sample_rate);
    REQUIRE(decoded.num_channels() == source.num_channels());
    // An M4A or CAF packet table trims encoder priming and padding, so the
    // decoded length matches the source to within one AAC frame. ADTS has no
    // such table, so its caller allows the 2112 priming frames as well.
    const auto in_frames = static_cast<std::int64_t>(source.num_frames());
    const auto out_frames = static_cast<std::int64_t>(decoded.num_frames());
    REQUIRE(std::llabs(out_frames - in_frames) <= max_length_error);
    for (std::uint32_t c = 0; c < source.num_channels(); ++c) {
        const auto& in = source.channels[c];
        const auto& out = decoded.channels[c];
        const std::size_t begin = in.size() / 4;
        const std::size_t end = std::min(in.size(), out.size()) * 3 / 4;
        const double expected = rms(in, begin, end);
        REQUIRE(std::abs(rms(out, begin, end) - expected) < 0.1 * expected);
        REQUIRE(best_correlation(in, out) > 0.98);
    }
}

void require_close(const AudioFileData& source, const AudioFileData& decoded, double tolerance) {
    REQUIRE(decoded.sample_rate == source.sample_rate);
    REQUIRE(decoded.num_channels() == source.num_channels());
    REQUIRE(decoded.num_frames() == source.num_frames());
    double worst = 0.0;
    for (std::uint32_t c = 0; c < source.num_channels(); ++c)
        for (std::size_t f = 0; f < source.channels[c].size(); ++f)
            worst = std::max(worst, static_cast<double>(
                                        std::abs(decoded.channels[c][f] - source.channels[c][f])));
    REQUIRE(worst <= tolerance);
}

std::optional<AudioFileData> write_and_read(const AudioFileData& source,
                                            const SystemAudioWriteOptions& options,
                                            const std::string& extension) {
    TempFile file(extension);
    REQUIRE(write_system_audio_file(file.path.string(), source, options));
    REQUIRE(fs::file_size(file.path) > 0);
    return FormatRegistry::instance().read(file.path.string());
}

} // namespace

TEST_CASE("System codec writer refuses invalid input", "[audio][system-codec]") {
    TempFile file(".m4a");
    SystemAudioWriteOptions options;

    AudioFileData empty;
    empty.sample_rate = 48000;
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), empty, options));

    auto zero_rate = stereo_tone(48000, 1024);
    zero_rate.sample_rate = 0;
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), zero_rate, options));

    auto ragged = stereo_tone(48000, 1024);
    ragged.channels[1].pop_back();
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), ragged, options));

    REQUIRE_FALSE(write_system_audio_file("", stereo_tone(48000, 1024), options));
    REQUIRE_FALSE(fs::exists(file.path));

    // Combinations no platform writes.
    REQUIRE_FALSE(system_audio_can_write(SystemAudioContainer::AIFF, SystemAudioEncoding::AAC));
    REQUIRE_FALSE(system_audio_can_write(SystemAudioContainer::ADTS, SystemAudioEncoding::ALAC));
    REQUIRE_FALSE(
        system_audio_can_write(SystemAudioContainer::M4A, SystemAudioEncoding::LinearPCM));

    SystemAudioWriteOptions odd_depth;
    odd_depth.encoding = SystemAudioEncoding::ALAC;
    odd_depth.bit_depth = 12;
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), stereo_tone(48000, 1024), odd_depth));

    SystemAudioWriteOptions float_aiff;
    float_aiff.container = SystemAudioContainer::AIFF;
    float_aiff.encoding = SystemAudioEncoding::LinearPCM;
    float_aiff.float_pcm = true;
    REQUIRE_FALSE(
        write_system_audio_file(file.path.string(), stereo_tone(48000, 1024), float_aiff));
    REQUIRE_FALSE(fs::exists(file.path));
}

#if defined(__APPLE__)

TEST_CASE("System codec writes AAC that reads back as the same signal", "[audio][system-codec]") {
    const auto source = stereo_tone(48000, 48000);
    SystemAudioWriteOptions options;
    REQUIRE(system_audio_can_write(options.container, options.encoding));

    const auto m4a = write_and_read(source, options, ".m4a");
    REQUIRE(m4a);
    require_lossy_match(source, *m4a);

    options.container = SystemAudioContainer::ADTS;
    const auto adts = write_and_read(source, options, ".aac");
    REQUIRE(adts);
    require_lossy_match(source, *adts, 2112 + 1024);
}

TEST_CASE("System codec AAC bitrate option changes the encoded size", "[audio][system-codec]") {
    // Ten seconds: a short M4A carries a fixed overhead of tens of kilobytes
    // that would hide the bitrate difference.
    const auto source = stereo_tone(48000, 480000);
    SystemAudioWriteOptions low;
    low.aac_bitrate = 64000;
    SystemAudioWriteOptions high;
    high.aac_bitrate = 256000;
    TempFile low_file(".m4a");
    TempFile high_file(".m4a");
    REQUIRE(write_system_audio_file(low_file.path.string(), source, low));
    REQUIRE(write_system_audio_file(high_file.path.string(), source, high));
    REQUIRE(fs::file_size(high_file.path) > fs::file_size(low_file.path) * 2);
}

TEST_CASE("System codec writes lossless ALAC, PCM CAF and 24-bit AIFF", "[audio][system-codec]") {
    const auto source = stereo_tone(44100, 10000);

    SystemAudioWriteOptions alac16;
    alac16.encoding = SystemAudioEncoding::ALAC;
    alac16.bit_depth = 16;
    const auto m4a = write_and_read(source, alac16, ".m4a");
    REQUIRE(m4a);
    require_close(source, *m4a, 1.5 / 32768.0);

    SystemAudioWriteOptions alac24;
    alac24.container = SystemAudioContainer::CAF;
    alac24.encoding = SystemAudioEncoding::ALAC;
    alac24.bit_depth = 24;
    const auto caf_alac = write_and_read(source, alac24, ".caf");
    REQUIRE(caf_alac);
    require_close(source, *caf_alac, 1.5 / 8388608.0);

    SystemAudioWriteOptions caf_float;
    caf_float.container = SystemAudioContainer::CAF;
    caf_float.encoding = SystemAudioEncoding::LinearPCM;
    caf_float.float_pcm = true;
    const auto caf = write_and_read(source, caf_float, ".caf");
    REQUIRE(caf);
    require_close(source, *caf, 0.0);

    // The built-in AIFF reader decodes what ExtAudioFile wrote.
    SystemAudioWriteOptions aiff24;
    aiff24.container = SystemAudioContainer::AIFF;
    aiff24.encoding = SystemAudioEncoding::LinearPCM;
    aiff24.bit_depth = 24;
    const auto aiff = write_and_read(source, aiff24, ".aiff");
    REQUIRE(aiff);
    require_close(source, *aiff, 1.5 / 8388608.0);
}

TEST_CASE("FormatRegistry writes .m4a, .aac, .alac and .caf through the system codec",
          "[audio][system-codec]") {
    auto& registry = FormatRegistry::instance();
    const auto source = stereo_tone(48000, 24000);
    for (const char* extension : {".m4a", ".aac", ".alac", ".caf"}) {
        INFO(extension);
        REQUIRE(registry.find_writer(extension) != nullptr);
        TempFile file(extension);
        REQUIRE(registry.write(file.path.string(), source));
        const auto decoded = registry.read(file.path.string());
        REQUIRE(decoded);
        REQUIRE(decoded->num_channels() == 2);
        REQUIRE(decoded->sample_rate == 48000);
    }
    // The built-in AIFF writer keeps .aiff.
    REQUIRE(registry.find_writer(".aiff")->format_name() == "AIFF");
}

TEST_CASE("A refused system codec write leaves the existing file untouched",
          "[audio][system-codec]") {
    // Arbitrary binary content, so a truncation, partial overwrite or
    // re-encode of the old file would all show as a difference.
    std::vector<char> previous(4099);
    for (std::size_t i = 0; i < previous.size(); ++i)
        previous[i] = static_cast<char>((i * 131u + 7u) & 0xFFu);
    TempFile file(".m4a");
    {
        std::ofstream out(file.path, std::ios::binary);
        out.write(previous.data(), static_cast<std::streamsize>(previous.size()));
    }
    const auto read_back = [&] {
        std::ifstream in(file.path, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    };

    // AAC-LC does not accept 192 kHz, so the encoder refuses after the
    // container would have been created; a 12-bit ALAC request is refused
    // before any file is touched.
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), stereo_tone(192000, 4096),
                                          SystemAudioWriteOptions{}));
    REQUIRE(read_back() == previous);
    SystemAudioWriteOptions odd_depth;
    odd_depth.encoding = SystemAudioEncoding::ALAC;
    odd_depth.bit_depth = 12;
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), stereo_tone(48000, 4096), odd_depth));
    REQUIRE(read_back() == previous);

    // No partial file is left beside it.
    for (const auto& entry : fs::directory_iterator(file.path.parent_path()))
        REQUIRE(entry.path().filename().string().find(file.path.stem().string() +
                                                      ".pulp-partial-") == std::string::npos);

    // A successful write replaces it.
    REQUIRE(write_system_audio_file(file.path.string(), stereo_tone(48000, 4096),
                                    SystemAudioWriteOptions{}));
    REQUIRE(read_back() != previous);
    REQUIRE(FormatRegistry::instance().read(file.path.string()));
}

TEST_CASE("System codec rounds an AAC bitrate to one the encoder accepts",
          "[audio][system-codec]") {
    const auto source = stereo_tone(48000, 24000);
    for (std::uint32_t bitrate : {1000u, 1000000u}) {
        INFO(bitrate);
        SystemAudioWriteOptions options;
        options.aac_bitrate = bitrate;
        TempFile file(".m4a");
        REQUIRE(write_system_audio_file(file.path.string(), source, options));
        REQUIRE(FormatRegistry::instance().read(file.path.string()));
    }
}

TEST_CASE("The system codec writer reports its capabilities", "[audio][system-codec]") {
    const auto writers = FormatRegistry::instance().writer_capabilities();
    const auto it = std::find_if(writers.begin(), writers.end(), [](const FormatCapabilities& c) {
        return c.format_name == "System codec";
    });
    REQUIRE(it != writers.end());
    REQUIRE(it->extensions == std::vector<std::string>{".m4a", ".aac", ".alac", ".caf"});
    REQUIRE(it->compressed);
    REQUIRE_FALSE(it->lossless);
    REQUIRE(it->float_samples);
    REQUIRE(it->bit_depths == std::vector<std::uint32_t>{16});
    REQUIRE(it->quality_options.size() == 1);
}

TEST_CASE("The system codec writer's advertised limits match what it accepts and refuses",
          "[audio][system-codec]") {
    const auto writers = FormatRegistry::instance().writer_capabilities();
    const auto it = std::find_if(writers.begin(), writers.end(), [](const FormatCapabilities& c) {
        return c.format_name == "System codec";
    });
    REQUIRE(it != writers.end());
    REQUIRE_FALSE(it->sample_rates.empty());
    REQUIRE(it->max_channels == 8);

    // Every advertised rate is written by every extension the entry covers.
    for (const auto& extension : it->extensions) {
        for (std::uint32_t rate : it->sample_rates) {
            INFO(extension << " at " << rate);
            TempFile file(extension);
            REQUIRE(FormatRegistry::instance().write(file.path.string(), stereo_tone(rate, 4096)));
        }
        // The most channels advertised are written, and one more is refused.
        for (std::uint32_t count : {it->max_channels, it->max_channels + 1}) {
            INFO(extension << " with " << count << " channels");
            AudioFileData data = stereo_tone(48000, 4096);
            data.channels.resize(count, data.channels.front());
            TempFile file(extension);
            const bool wrote = FormatRegistry::instance().write(file.path.string(), data);
            if (count <= it->max_channels)
                REQUIRE(wrote);
            else if (extension == ".m4a" || extension == ".aac")
                REQUIRE_FALSE(wrote);
        }
    }

    // The entry states the weakest case, so a rate AAC refuses is not advertised.
    for (std::uint32_t refused : {64000u, 96000u, 192000u}) {
        INFO(refused);
        REQUIRE(std::find(it->sample_rates.begin(), it->sample_rates.end(), refused) ==
                it->sample_rates.end());
        TempFile file(".m4a");
        REQUIRE_FALSE(
            FormatRegistry::instance().write(file.path.string(), stereo_tone(refused, 4096)));
    }
}

TEST_CASE("The CoreAudio reader reads rates and channel counts the AAC writer refuses",
          "[audio][system-codec]") {
    // The reader decodes whatever the file holds, so its report leaves rates
    // and channels unrestricted rather than stating the AAC writer's limits.
    const auto readers = FormatRegistry::instance().reader_capabilities();
    const auto it = std::find_if(readers.begin(), readers.end(), [](const FormatCapabilities& c) {
        return c.format_name == "CoreAudio";
    });
    REQUIRE(it != readers.end());
    REQUIRE(it->sample_rates.empty());
    REQUIRE(it->max_channels == 0);

    TempFile alac(".alac");
    REQUIRE(FormatRegistry::instance().write(alac.path.string(), stereo_tone(96000, 4096)));
    const auto high = FormatRegistry::instance().read(alac.path.string());
    REQUIRE(high);
    REQUIRE(high->sample_rate == 96000);

    AudioFileData wide = stereo_tone(48000, 4096);
    wide.channels.resize(9, wide.channels.front());
    TempFile caf(".caf");
    REQUIRE(FormatRegistry::instance().write(caf.path.string(), wide));
    const auto many = FormatRegistry::instance().read(caf.path.string());
    REQUIRE(many);
    REQUIRE(many->num_channels() == 9);
}

#elif defined(_WIN32)

TEST_CASE("System codec writes AAC in M4A that Media Foundation reads back",
          "[audio][system-codec]") {
    REQUIRE(system_audio_can_write(SystemAudioContainer::M4A, SystemAudioEncoding::AAC));
    REQUIRE_FALSE(system_audio_can_write(SystemAudioContainer::CAF, SystemAudioEncoding::ALAC));
    const auto source = stereo_tone(48000, 48000);
    const auto decoded = write_and_read(source, SystemAudioWriteOptions{}, ".m4a");
    REQUIRE(decoded);
    require_lossy_match(source, *decoded);

    // The Windows encoder is offered at 44.1 and 48 kHz, mono or stereo.
    TempFile file(".m4a");
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), stereo_tone(22050, 4096),
                                          SystemAudioWriteOptions{}));
    AudioFileData six = stereo_tone(48000, 4096);
    six.channels.resize(6, six.channels.front());
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), six, SystemAudioWriteOptions{}));

    const auto readers = FormatRegistry::instance().reader_capabilities();
    REQUIRE(std::any_of(readers.begin(), readers.end(), [](const FormatCapabilities& c) {
        return c.format_name == "Media Foundation" && !c.extensions.empty();
    }));
}

#else

TEST_CASE("Platforms without a system codec refuse every write", "[audio][system-codec]") {
    for (auto container : {SystemAudioContainer::M4A, SystemAudioContainer::ADTS,
                           SystemAudioContainer::CAF, SystemAudioContainer::AIFF})
        for (auto encoding :
             {SystemAudioEncoding::AAC, SystemAudioEncoding::ALAC, SystemAudioEncoding::LinearPCM})
            REQUIRE_FALSE(system_audio_can_write(container, encoding));
    TempFile file(".m4a");
    REQUIRE_FALSE(write_system_audio_file(file.path.string(), stereo_tone(48000, 1024),
                                          SystemAudioWriteOptions{}));
    REQUIRE(FormatRegistry::instance().find_writer(".caf") == nullptr);
}

#endif
