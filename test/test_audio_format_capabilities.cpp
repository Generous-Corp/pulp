// Format capability reports: every registered reader and writer describes
// itself consistently with what it accepts, and each built-in format reports
// the limits its codec actually has. Writers are checked by writing a file
// and reading its header back.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/format_registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::audio;

namespace {

std::optional<FormatCapabilities> find_caps(const std::vector<FormatCapabilities>& list,
                                            const std::string& name) {
    for (const auto& caps : list)
        if (caps.format_name == name)
            return caps;
    return std::nullopt;
}

bool contains(const std::vector<std::uint32_t>& list, std::uint32_t value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

AudioFileData tone(std::uint32_t channels, std::uint32_t frames, std::uint32_t rate) {
    AudioFileData data;
    data.sample_rate = rate;
    data.channels.assign(channels, std::vector<float>(frames));
    for (std::uint32_t c = 0; c < channels; ++c)
        for (std::uint32_t f = 0; f < frames; ++f)
            data.channels[c][f] =
                0.25f * static_cast<float>(std::sin(0.05 * static_cast<double>(f) + c));
    return data;
}

fs::path temp_path(const std::string& extension) {
    // CTest runs each case as its own process, in parallel, so the name must
    // be unique across processes, not only within one.
    static const auto process_tag =
        std::to_string(std::random_device{}()) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    static std::atomic<int> counter{0};
    return fs::temp_directory_path() / ("pulp_format_caps_" + process_tag + "_" +
                                        std::to_string(counter.fetch_add(1)) + extension);
}

template <typename Handler>
void require_consistent(const Handler& handler, const FormatCapabilities& caps) {
    REQUIRE(caps.format_name == handler.format_name());
    REQUIRE_FALSE(caps.extensions.empty());
    for (const auto& ext : caps.extensions) {
        REQUIRE(ext.size() > 1);
        REQUIRE(ext.front() == '.');
        REQUIRE(std::none_of(ext.begin(), ext.end(), [](char c) { return c >= 'A' && c <= 'Z'; }));
        REQUIRE(handler.supports_extension(ext));
    }
    REQUIRE(std::is_sorted(caps.bit_depths.begin(), caps.bit_depths.end()));
    REQUIRE(std::is_sorted(caps.sample_rates.begin(), caps.sample_rates.end()));
    if (!caps.compressed)
        REQUIRE(caps.lossless);
    if (!caps.lossless)
        REQUIRE(caps.compressed);
}

class NameOnlyReader : public FormatReader {
  public:
    std::optional<AudioFileInfo> read_info(const std::string&) override {
        return std::nullopt;
    }
    std::optional<AudioFileData> read(const std::string&) override {
        return std::nullopt;
    }
    bool supports_extension(std::string_view ext) const override {
        return ext == ".nameonly";
    }
    std::string format_name() const override {
        return "NameOnly";
    }
};

} // namespace

// A report with no extensions is a handler without a capabilities()
// override, which reports only its name by contract; there is nothing else to
// check. Every other report must agree with its handler.
static void require_listed_extensions_resolve(const std::vector<FormatCapabilities>& list,
                                              bool readers) {
    auto& registry = FormatRegistry::instance();
    for (const auto& caps : list) {
        INFO(caps.format_name);
        REQUIRE_FALSE(caps.format_name.empty());
        for (const auto& ext : caps.extensions) {
            INFO(ext);
            if (readers)
                REQUIRE(registry.find_reader(ext) != nullptr);
            else
                REQUIRE(registry.find_writer(ext) != nullptr);
        }
    }
}

TEST_CASE("Every registered reader and writer reports consistent capabilities",
          "[audio][format-capabilities]") {
    auto& registry = FormatRegistry::instance();
    const auto readers = registry.reader_capabilities();
    const auto writers = registry.writer_capabilities();
    REQUIRE_FALSE(readers.empty());
    REQUIRE_FALSE(writers.empty());
    require_listed_extensions_resolve(readers, true);
    require_listed_extensions_resolve(writers, false);

    // The handler that wins each extension agrees with its own predicates,
    // unless it reports its name only.
    for (const auto& ext : registry.supported_read_extensions()) {
        auto* reader = registry.find_reader(ext);
        REQUIRE(reader != nullptr);
        INFO(ext);
        const auto caps = reader->capabilities();
        if (!caps.extensions.empty())
            require_consistent(*reader, caps);
    }
    for (const auto& ext : registry.supported_write_extensions()) {
        auto* writer = registry.find_writer(ext);
        REQUIRE(writer != nullptr);
        INFO(ext);
        const auto caps = writer->capabilities();
        if (!caps.extensions.empty())
            require_consistent(*writer, caps);
    }

    // The built-in handlers always report in full.
    std::vector<std::string> full_readers = {"WAV", "FLAC", "MP3", "OGG Vorbis", "AIFF"};
#ifdef __APPLE__
    full_readers.push_back("CoreAudio");
#endif
    for (const auto& name : full_readers) {
        INFO(name);
        const auto caps = find_caps(readers, name);
        REQUIRE(caps);
        REQUIRE_FALSE(caps->extensions.empty());
    }
    for (const auto& name : {std::string("WAV"), std::string("AIFF")}) {
        INFO(name);
        const auto caps = find_caps(writers, name);
        REQUIRE(caps);
        REQUIRE_FALSE(caps->extensions.empty());
    }
}

TEST_CASE("Built-in readers report their codec limits", "[audio][format-capabilities]") {
    const auto readers = FormatRegistry::instance().reader_capabilities();

    const auto wav = find_caps(readers, "WAV");
    REQUIRE(wav);
    REQUIRE(wav->bit_depths == std::vector<std::uint32_t>{8, 16, 24, 32});
    REQUIRE(wav->float_samples);
    REQUIRE_FALSE(wav->compressed);
    REQUIRE(wav->sample_rates.empty());

    const auto flac = find_caps(readers, "FLAC");
    REQUIRE(flac);
    REQUIRE(flac->compressed);
    REQUIRE(flac->lossless);
    REQUIRE(flac->max_channels == 8);
    // FLAC allows every depth from 4 to 32 bits, and dr_flac decodes them all.
    REQUIRE(flac->bit_depths.size() == 29);
    REQUIRE(flac->bit_depths.front() == 4);
    REQUIRE(flac->bit_depths.back() == 32);

    const auto mp3 = find_caps(readers, "MP3");
    REQUIRE(mp3);
    REQUIRE(mp3->compressed);
    REQUIRE_FALSE(mp3->lossless);
    REQUIRE(mp3->bit_depths.empty());
    REQUIRE(mp3->max_channels == 2);
    REQUIRE(contains(mp3->sample_rates, 44100));
    REQUIRE_FALSE(contains(mp3->sample_rates, 96000));

    const auto ogg = find_caps(readers, "OGG Vorbis");
    REQUIRE(ogg);
    REQUIRE(ogg->compressed);
    REQUIRE_FALSE(ogg->lossless);
    REQUIRE(ogg->max_channels == 255);

    const auto aiff = find_caps(readers, "AIFF");
    REQUIRE(aiff);
    REQUIRE_FALSE(aiff->compressed);
    REQUIRE(aiff->bit_depths == std::vector<std::uint32_t>{8, 16, 24, 32});

#ifdef __APPLE__
    const auto platform = find_caps(readers, "CoreAudio");
    REQUIRE(platform);
    REQUIRE(platform->compressed);
    REQUIRE(std::find(platform->extensions.begin(), platform->extensions.end(), ".m4a") !=
            platform->extensions.end());
#endif
}

TEST_CASE("Uncompressed writers write the bit depth they report", "[audio][format-capabilities]") {
    auto& registry = FormatRegistry::instance();
    for (const char* name : {"WAV", "AIFF"}) {
        INFO(name);
        const auto caps = find_caps(registry.writer_capabilities(), name);
        REQUIRE(caps);
        REQUIRE(caps->bit_depths == std::vector<std::uint32_t>{16});
        REQUIRE_FALSE(caps->compressed);
        REQUIRE(caps->quality_options.empty());

        const auto path = temp_path(caps->extensions.front());
        REQUIRE(registry.write(path.string(), tone(2, 256, 48000)));
        const auto info = registry.read_info(path.string());
        fs::remove(path);
        REQUIRE(info);
        REQUIRE(contains(caps->bit_depths, info->bits_per_sample));
        REQUIRE(info->num_channels == 2);
    }
}

TEST_CASE("Optional encoders report their settings and write within them",
          "[audio][format-capabilities]") {
    auto& registry = FormatRegistry::instance();
    const auto writers = registry.writer_capabilities();

    if (const auto flac = find_caps(writers, "FLAC")) {
        REQUIRE(flac->compressed);
        REQUIRE(flac->lossless);
        REQUIRE(flac->bit_depths == std::vector<std::uint32_t>{16});
        REQUIRE(flac->quality_options.size() == 1);
        REQUIRE(flac->quality_options[0].name == "compression_level");
        REQUIRE(flac->quality_options[0].default_value == "5");
        const auto path = temp_path(".flac");
        REQUIRE(registry.write(path.string(), tone(2, 4096, 44100)));
        const auto info = registry.read_info(path.string());
        fs::remove(path);
        REQUIRE(info);
        REQUIRE(info->bits_per_sample == 16);
    }

    if (const auto mp3 = find_caps(writers, "MP3")) {
        REQUIRE_FALSE(mp3->lossless);
        REQUIRE(mp3->max_channels == 2);
        REQUIRE(contains(mp3->sample_rates, 44100));
        REQUIRE(mp3->quality_options.size() == 2);
        const auto path = temp_path(".mp3");
        REQUIRE(registry.write(path.string(), tone(2, 4096, 44100)));
        const auto info = registry.read_info(path.string());
        fs::remove(path);
        REQUIRE(info);
        REQUIRE(info->num_channels == 2);
    }

    if (const auto aac = find_caps(writers, "AAC")) {
        REQUIRE_FALSE(aac->lossless);
        REQUIRE(aac->max_channels == 2);
        REQUIRE(contains(aac->sample_rates, 48000));
        REQUIRE(aac->quality_options.size() == 3);
    }

    if (const auto alac = find_caps(writers, "ALAC")) {
        REQUIRE(alac->compressed);
        REQUIRE(alac->lossless);
        REQUIRE(alac->bit_depths == std::vector<std::uint32_t>{16});
    }
}

TEST_CASE("A reader without a capabilities override reports only its name",
          "[audio][format-capabilities]") {
    NameOnlyReader reader;
    const auto caps = reader.capabilities();
    REQUIRE(caps.format_name == "NameOnly");
    REQUIRE(caps.extensions.empty());
    REQUIRE(caps.bit_depths.empty());
    REQUIRE(caps.sample_rates.empty());
    REQUIRE(caps.max_channels == 0);
    REQUIRE_FALSE(caps.compressed);
    REQUIRE(caps.lossless);
}
