// Layout report over byte fixtures: parity with the in-memory decoder,
// standard-profile classification, the warning taxonomy, bounded reads on a
// sparse multi-gigabyte file, raw PCM, redaction, and the container walks
// over the checked-in fixture files.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/audio/audio_file_layout.hpp>
#include <pulp/audio/audio_file_layout_json.hpp>
#include <pulp/audio/wav_decoder.hpp>

#include <choc/text/choc_JSON.h>

#include "support/fixture_root.hpp"
#include "support/wav_fixture_builders.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace pulp_test::wav;
using namespace pulp::audio;
namespace fs = std::filesystem;

namespace {

bool has_warning(const AudioFileLayoutReport& report, LayoutWarningKind kind) {
    return std::any_of(report.warnings.begin(), report.warnings.end(),
                       [kind](const LayoutWarning& w) { return w.kind == kind; });
}

const LayoutRegion* find_region(const AudioFileLayoutReport& report, const std::string& id) {
    for (const auto& region : report.regions)
        if (region.id == id)
            return &region;
    return nullptr;
}

std::set<std::string> top_level_keys(const std::string& json) {
    const auto parsed = choc::json::parse(json);
    std::set<std::string> keys;
    for (std::uint32_t i = 0; i < parsed.size(); ++i)
        keys.insert(std::string(parsed.getObjectMemberAt(i).name));
    return keys;
}

Bytes two_frames_mono16() {
    Bytes data;
    append_sample(data, std::int16_t{0});
    append_sample(data, std::int16_t{1});
    return data;
}

fs::path temp_file(const std::string& name) {
    return fs::temp_directory_path() / ("pulp-audio-file-layout-" + std::to_string(::getpid()) + "-" + name);
}

std::uint64_t resident_bytes() {
#if defined(__APPLE__)
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#elif !defined(_WIN32)
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
#else
    return 0u;
#endif
}

} // namespace

TEST_CASE("layout report agrees with inspect_wav on every decoder fixture", "[audio][file-layout][parity]") {
    // The decoder is the ground truth for accept/reject and for the stream
    // facts; the report must never disagree, including on every truncated
    // prefix of a valid file.
    std::size_t accepted = 0u;
    std::size_t rejected = 0u;
    auto check = [&](const std::string& name, std::span<const std::uint8_t> bytes) {
        INFO(name << " (" << bytes.size() << " bytes)");
        const auto info = inspect_wav(bytes);
        const auto report = inspect_audio_file_layout(bytes);
        if (report.container != AudioContainer::wav) {
            CHECK_FALSE(info);
            ++rejected;
            return;
        }
        REQUIRE(report.decode);
        CHECK(report.decode->decodable == info.has_value());
        if (info) {
            ++accepted;
            REQUIRE(report.stream);
            CHECK(report.stream->sample_rate == info->sample_rate);
            CHECK(report.stream->channels == info->num_channels);
            CHECK(report.stream->bits_per_sample == info->bits_per_sample);
            CHECK(report.stream->frames == info->num_frames);
        } else {
            ++rejected;
            CHECK_FALSE(report.decode->reason.empty());
        }
    };
    for (const auto& fixture : decoder_fixtures())
        check(fixture.name, fixture.bytes);
    const auto valid = make_wav(make_format(1u, 1u, 48'000u, 16u), two_frames_mono16());
    for (std::size_t length = 0u; length < valid.size(); ++length)
        check("prefix " + std::to_string(length), std::span(valid).first(length));
    // The sweep must have exercised both outcomes or it proved nothing.
    CHECK(accepted >= 9u);
    CHECK(rejected >= 50u);
}

TEST_CASE("interpret_wav_format reproduces the decoder's facts from a fmt payload", "[audio][file-layout]") {
    const auto facts = interpret_wav_format(make_extensible_pcm_format(2u, 44'100u, 32u, 24u), 16u);
    REQUIRE(facts);
    CHECK(facts->format_tag == 0xfffeu);
    CHECK(facts->translated_format_tag == 1u);
    CHECK(facts->channels == 2u);
    CHECK(facts->sample_rate == 44'100u);
    CHECK(facts->bits_per_sample == 32u);
    CHECK(facts->bytes_per_frame == 8u);
    CHECK(facts->total_frames == 2u);
    // Past 4 GiB the synthetic header switches to RF64 and the count survives.
    const auto large = interpret_wav_format(make_format(1u, 1u, 48'000u, 16u), 5'000'000'000ull);
    REQUIRE(large);
    CHECK(large->total_frames == 2'500'000'000ull);
    CHECK_FALSE(interpret_wav_format(Bytes{1u, 0u, 1u, 0u}, 16u));
}

TEST_CASE("standard headers classify standard with no warning", "[audio][file-layout][profile]") {
    SECTION("44-byte PCM") {
        const auto report = inspect_audio_file_layout(make_wav(make_format(1u, 1u, 48'000u, 16u), two_frames_mono16()));
        CHECK(report.container == AudioContainer::wav);
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::pcm44);
        CHECK(report.is_standard());
        CHECK(report.regions.size() == 3u);
        // The canonical 44-byte header: data payload begins at byte 44.
        CHECK(report.regions[2].offset == 36u);
        CHECK(report.regions[2].declared_size == 4u);
        CHECK(report.regions[2].actual_size == 4u);
        CHECK(report.file_size == 48u);
    }
    SECTION("BWF with bext before data") {
        Bytes bext(602u, 0u);
        bext[346] = 2u;  // version 2 carries loudness fields
        const auto report = inspect_audio_file_layout(make_wav_with_chunks(
            make_format(1u, 1u, 48'000u, 16u), two_frames_mono16(), {{"bext", bext}}));
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::bwf);
        CHECK(report.is_standard());
        REQUIRE(report.decode);
        CHECK(report.decode->decodable);
        const auto* bext_region = find_region(report, "bext");
        REQUIRE(bext_region);
        CHECK(bext_region->kind == LayoutRegionKind::metadata);
        CHECK(std::any_of(bext_region->fields.begin(), bext_region->fields.end(),
                          [](const LayoutField& f) { return f.name == "loudness_value"; }));
        REQUIRE(report.metadata_present.size() == 1u);
        CHECK(report.metadata_present[0].id == "bext");
    }
    SECTION("float32 with an 18-byte fmt") {
        Bytes data;
        append_sample(data, 0.5f);
        const auto report = inspect_audio_file_layout(make_wav(make_format_18(3u, 1u, 48'000u, 32u), data));
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::floating);
        CHECK(report.is_standard());
        REQUIRE(report.stream);
        CHECK(report.stream->encoding == "float");
        CHECK(report.stream->frames == 1u);
        REQUIRE(report.decode);
        CHECK(report.decode->decodable);
        const auto* fmt = find_region(report, "fmt ");
        REQUIRE(fmt);
        CHECK(fmt->declared_size == 18u);
    }
    SECTION("extensible 24-in-32") {
        Bytes data;
        append_sample(data, std::int32_t{0x12345600});
        append_sample(data, std::int32_t{0});
        const auto report = inspect_audio_file_layout(
            make_wav(make_extensible_pcm_format(2u, 96'000u, 32u, 24u), data));
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::extensible);
        CHECK(report.is_standard());
        REQUIRE(report.stream);
        CHECK(report.stream->bits_per_sample == 32u);
        CHECK(report.stream->valid_bits == 24u);
        CHECK(report.stream->channels == 2u);
        CHECK(report.stream->frames == 1u);
    }
    SECTION("RF64 with ds64") {
        const auto report = inspect_audio_file_layout(make_rf64(make_format(1u, 1u, 48'000u, 16u), two_frames_mono16()));
        CHECK(report.container_detail == "RF64");
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::rf64);
        CHECK(report.is_standard());
        REQUIRE(report.stream);
        CHECK(report.stream->frames == 2u);
        const auto* ds64 = find_region(report, "ds64");
        REQUIRE(ds64);
        CHECK(ds64->kind == LayoutRegionKind::envelope);
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_THAT(report.decode->reason, Catch::Matchers::ContainsSubstring("RIFF envelopes only"));
    }
    SECTION("Wave64") {
        const auto report = inspect_audio_file_layout(make_w64(make_format(1u, 2u, 44'100u, 16u), Bytes(8u, 0u)));
        CHECK(report.container == AudioContainer::wav);
        CHECK(report.container_detail == "W64");
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::w64);
        CHECK(report.is_standard());
        REQUIRE(report.stream);
        CHECK(report.stream->channels == 2u);
        CHECK(report.stream->frames == 2u);
        const auto* data = find_region(report, "data");
        REQUIRE(data);
        CHECK(data->declared_size == 8u);
        CHECK(data->offset == 40u + 24u + 16u);
    }
}

TEST_CASE("specification violations each raise exactly their warning", "[audio][file-layout][warnings]") {
    const auto format = make_format(1u, 1u, 48'000u, 16u);
    const auto data = two_frames_mono16();

    SECTION("odd chunk at end of file without its pad byte") {
        auto bytes = make_wav(format, data);
        append_id(bytes, "JUNK");
        append_u32(bytes, 1u);
        bytes.push_back(0x55u);
        set_u32(bytes, 4u, static_cast<std::uint32_t>(bytes.size() - 8u));
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::missing_pad_byte));
        CHECK(report.warnings.size() == 1u);
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_FALSE(inspect_wav(bytes));
    }
    SECTION("duplicate fmt") {
        const auto report = inspect_audio_file_layout(make_wav_with_chunks(format, data, {{"fmt ", format}}));
        CHECK(has_warning(report, LayoutWarningKind::duplicate_fmt));
        CHECK(report.warnings.size() == 1u);
        REQUIRE(report.decode);
        CHECK(report.decode->decodable);
    }
    SECTION("fmt after data") {
        const auto bytes = make_wav(format, data, true);
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::fmt_after_data));
        CHECK(report.warnings.size() == 1u);
        REQUIRE(report.decode);
        CHECK(report.decode->decodable == inspect_wav(bytes).has_value());
        CHECK(report.decode->decodable);
    }
    SECTION("data past end of file") {
        auto bytes = make_wav(format, data);
        set_u32(bytes, 40u, 400u);
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::truncated_data));
        CHECK_FALSE(has_warning(report, LayoutWarningKind::chunk_past_eof));
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        const auto* region = find_region(report, "data");
        REQUIRE(region);
        CHECK(region->declared_size == 400u);
        CHECK(region->actual_size == 4u);
    }
    SECTION("metadata chunk past end of file") {
        auto bytes = make_wav_with_chunks(format, data, {}, {{"LIST", Bytes{'I', 'N', 'F', 'O'}}});
        set_u32(bytes, bytes.size() - 8u, 4000u);
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::chunk_past_eof));
        CHECK_FALSE(has_warning(report, LayoutWarningKind::truncated_data));
    }
    SECTION("data size zero") {
        Bytes samples(8u, 0u);
        auto bytes = make_wav(format, samples);
        set_u32(bytes, 40u, 0u);
        const auto report = inspect_audio_file_layout(bytes);
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_THAT(report.decode->reason, Catch::Matchers::ContainsSubstring("zero frames"));
        // The sample bytes that follow parse as chunks the file never declared.
        CHECK(std::any_of(report.regions.begin(), report.regions.end(),
                          [](const LayoutRegion& r) { return r.kind == LayoutRegionKind::unknown; }));
    }
    SECTION("sentinel size without ds64") {
        auto bytes = make_wav(format, data);
        set_u32(bytes, 40u, 0xffffffffu);
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::sentinel_size_without_ds64));
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_FALSE(inspect_wav(bytes));
    }
    SECTION("RF64 ds64 data size overlapping the following chunk") {
        const auto bytes = make_rf64(format, data, static_cast<std::uint32_t>(data.size()), data.size() + 8u,
                                     {{"JUNK", Bytes{1u, 2u, 3u, 4u}}});
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::chunk_overlap));
        REQUIRE(find_region(report, "JUNK"));
    }
    SECTION("extensible channel mask disagreeing with the channel count") {
        const auto report = inspect_audio_file_layout(
            make_wav(make_extensible_pcm_format(2u, 48'000u, 16u, 16u, 0x3fu), Bytes(4u, 0u)));
        CHECK(has_warning(report, LayoutWarningKind::extensible_mask_channel_mismatch));
        CHECK(report.warnings.size() == 1u);
    }
    SECTION("valid bits above the container width") {
        const auto report = inspect_audio_file_layout(
            make_wav(make_extensible_pcm_format(1u, 48'000u, 16u, 24u), Bytes(2u, 0u)));
        CHECK(has_warning(report, LayoutWarningKind::valid_bits_above_container));
        CHECK(report.warnings.size() == 1u);
    }
    SECTION("envelope size mismatch in both directions") {
        auto larger = make_wav(format, data);
        set_u32(larger, 4u, static_cast<std::uint32_t>(larger.size()));
        CHECK(has_warning(inspect_audio_file_layout(larger), LayoutWarningKind::envelope_size_mismatch));
        auto trailing = make_wav(format, data);
        append_chunk(trailing, "JUNK", Bytes{1u, 2u});
        const auto report = inspect_audio_file_layout(trailing);
        CHECK(has_warning(report, LayoutWarningKind::envelope_size_mismatch));
        CHECK(report.regions.back().note == "bytes after the declared envelope");
    }
    SECTION("LIST-INFO heavy file is standard and lists its entries") {
        Bytes list{'I', 'N', 'F', 'O'};
        for (int i = 0; i < 20; ++i) {
            const std::string tag = "IN" + std::to_string(10 + i);
            for (char c : tag) list.push_back(static_cast<std::uint8_t>(c));
            const std::string value = "value-" + std::to_string(i);
            append_u32(list, static_cast<std::uint32_t>(value.size() + 1u));
            for (char c : value) list.push_back(static_cast<std::uint8_t>(c));
            list.push_back(0u);
            if ((list.size() & 1u) != 0u) list.push_back(0u);
        }
        const auto report = inspect_audio_file_layout(make_wav_with_chunks(format, data, {{"LIST", list}}));
        CHECK(report.is_standard());
        const auto* region = find_region(report, "LIST");
        REQUIRE(region);
        const auto entries = std::find_if(region->fields.begin(), region->fields.end(),
                                          [](const LayoutField& f) { return f.name == "entries"; });
        REQUIRE(entries != region->fields.end());
        CHECK(std::get<std::uint64_t>(entries->value) == 20u);
        CHECK(report.metadata_text.empty());
    }
}

TEST_CASE("declared frames past the decode limit are a fact, not an error", "[audio][file-layout][limits]") {
    // 100,000,001 mono 16-bit frames declared by the header of a file that
    // holds none of them. The limit fires on the declaration.
    auto bytes = make_wav(make_format(1u, 1u, 48'000u, 16u), Bytes{});
    set_u32(bytes, 40u, 200'000'002u);
    const auto report = inspect_audio_file_layout(bytes);
    REQUIRE(report.stream);
    CHECK(report.stream->frames == 100'000'001u);
    REQUIRE(report.decode_limits);
    CHECK(report.decode_limits->exceeds);
    REQUIRE(report.decode_limits->exceeded.size() == 1u);
    CHECK(report.decode_limits->exceeded[0] == "max_frames");
    CHECK(report.decode_limits->limits.max_frames == 100'000'000u);
    CHECK(report.error.empty());
    // One frame fewer is within the limit.
    set_u32(bytes, 40u, 200'000'000u);
    const auto within = inspect_audio_file_layout(bytes);
    REQUIRE(within.decode_limits);
    CHECK_FALSE(within.decode_limits->exceeds);
}

TEST_CASE("sparse multi-gigabyte WAV costs only header reads", "[audio][file-layout][memory]") {
    constexpr std::uint64_t kFileSize = 2'147'483'648ull + 44u;  // 2 GiB of samples behind a 44-byte header
    const auto path = temp_file("sparse.wav");
    {
        auto header = make_wav(make_format(1u, 2u, 48'000u, 16u), Bytes{});
        set_u32(header, 4u, static_cast<std::uint32_t>(kFileSize - 8u));
        set_u32(header, 40u, static_cast<std::uint32_t>(kFileSize - 44u));
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        out.close();
        fs::resize_file(path, kFileSize);
    }
    REQUIRE(fs::file_size(path) == kFileSize);

    const auto before = resident_bytes();
    FileByteSource file(path);
    REQUIRE(file.is_open());
    CountingByteSource counting(file);
    const auto report = inspect_audio_file_layout(counting);
    const auto after = resident_bytes();
    fs::remove(path);

    CHECK(report.file_size == kFileSize);
    CHECK(report.is_standard());
    REQUIRE(report.decode);
    CHECK(report.decode->decodable);
    REQUIRE(report.stream);
    CHECK(report.stream->frames == (kFileSize - 44u) / 4u);
    REQUIRE(report.decode_limits);
    CHECK(report.decode_limits->exceeds);
    // Only the envelope and three chunk headers were touched.
    CHECK(counting.bytes_read() < 1024u);
    CHECK(counting.read_calls() < 16u);
    CHECK(counting.high_water_offset() < 1024u);
    CHECK(report.bytes_read == counting.bytes_read());
    // Peak residency must not grow by anything like the file's size.
    CHECK(after - before < 32u * 1024u * 1024u);
}

TEST_CASE("sizes past 4 GiB survive the RF64 arithmetic", "[audio][file-layout][rf64]") {
    const auto format = make_format(1u, 2u, 48'000u, 16u);
    const auto bytes = make_rf64(format, Bytes{}, 0xffffffffu, 5'000'000'000ull);
    const auto report = inspect_audio_file_layout(bytes);
    REQUIRE(report.stream);
    CHECK(report.stream->frames == 1'250'000'000ull);
    CHECK(has_warning(report, LayoutWarningKind::truncated_data));
    const auto* data = find_region(report, "data");
    REQUIRE(data);
    CHECK(data->declared_size == 5'000'000'000ull);
    CHECK(data->actual_size == 0u);
    REQUIRE(report.decode_limits);
    CHECK(report.decode_limits->exceeds);
}

TEST_CASE("hostile headers are reported, never guessed around", "[audio][file-layout][hostile]") {
    const auto format = make_format(1u, 1u, 48'000u, 16u);
    const auto data = two_frames_mono16();
    SECTION("header shorter than 12 bytes") {
        const Bytes stub{'R', 'I', 'F', 'F', 4u, 0u, 0u, 0u};
        const auto report = inspect_audio_file_layout(stub);
        CHECK(report.container == AudioContainer::unknown);
        CHECK(report.file_size == 8u);
    }
    SECTION("empty input") {
        const auto report = inspect_audio_file_layout(Bytes{});
        CHECK(report.container == AudioContainer::unknown);
        CHECK(report.error == "empty input");
    }
    SECTION("RIFF size zero") {
        auto bytes = make_wav(format, data);
        set_u32(bytes, 4u, 0u);
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::envelope_size_mismatch));
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        // The walk still finds the chunks the file holds.
        REQUIRE(find_region(report, "data"));
        REQUIRE(report.stream);
        CHECK(report.stream->frames == 2u);
    }
    SECTION("ds64 present but the RIFF size is not the sentinel") {
        auto bytes = make_rf64(format, data);
        set_u32(bytes, 4u, static_cast<std::uint32_t>(bytes.size() - 8u));
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(report.is_standard());
        REQUIRE(report.profile);
        CHECK(*report.profile == WavProfile::rf64);
        REQUIRE(report.stream);
        CHECK(report.stream->frames == 2u);
    }
    SECTION("fmt shorter than 16 bytes") {
        const auto report = inspect_audio_file_layout(make_wav(Bytes{1u, 0u, 1u, 0u, 0x80u, 0xbbu, 0u, 0u}, data));
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_THAT(report.decode->reason, Catch::Matchers::ContainsSubstring("fmt"));
        CHECK_FALSE(report.stream);
    }
    SECTION("odd data chunk at end of file") {
        auto bytes = make_wav(format, Bytes{0u, 0u, 0u});
        bytes.pop_back();  // drop the pad byte make_wav added
        set_u32(bytes, 4u, static_cast<std::uint32_t>(bytes.size() - 8u));
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::missing_pad_byte));
        CHECK(has_warning(report, LayoutWarningKind::truncated_data));
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_FALSE(inspect_wav(bytes));
    }
    SECTION("unprintable chunk id") {
        auto bytes = make_wav_with_chunks(format, data, {{"JUNK", Bytes{1u, 2u}}});
        bytes[36] = 0x01u;
        const auto report = inspect_audio_file_layout(bytes);
        const auto it = std::find_if(report.regions.begin(), report.regions.end(),
                                     [](const LayoutRegion& r) { return r.offset == 36u; });
        REQUIRE(it != report.regions.end());
        CHECK(it->kind == LayoutRegionKind::unknown);
        CHECK_FALSE(it->note.empty());
    }
    SECTION("FLAC bytes are FLAC whatever the caller calls them") {
        Bytes flac{'f', 'L', 'a', 'C', 0x80u, 0u, 0u, 34u};
        flac.resize(8u + 34u, 0u);
        const auto report = inspect_audio_file_layout(flac);
        CHECK(report.container == AudioContainer::flac);
    }
}

TEST_CASE("raw PCM is described, never guessed", "[audio][file-layout][raw]") {
    LayoutInspectOptions options;
    RawPcmSpec spec;
    spec.format = RawPcmSpec::Format::s24;
    spec.endian = RawPcmSpec::Endian::big;
    spec.channels = 2u;
    spec.sample_rate = 44'100u;
    spec.offset = 4u;
    options.raw = spec;
    const Bytes bytes(4u + 6u * 3u + 4u, 0u);  // header, three stereo frames, four stray bytes
    const auto report = inspect_audio_file_layout(bytes, options);
    CHECK(report.container == AudioContainer::raw);
    REQUIRE(report.stream);
    CHECK(report.stream->frames == 3u);
    CHECK(report.stream->bits_per_sample == 24u);
    CHECK(report.stream->bytes_per_frame == 6u);
    REQUIRE(report.truncated_tail_bytes);
    CHECK(*report.truncated_tail_bytes == 4u);
    REQUIRE(report.regions.size() == 2u);
    CHECK(report.regions[1].offset == 4u);
    CHECK(report.is_standard());

    spec.offset = 100u;
    options.raw = spec;
    const auto past = inspect_audio_file_layout(bytes, options);
    CHECK_THAT(past.error, Catch::Matchers::ContainsSubstring("past the end"));
    spec.offset = 0u;
    spec.channels = 0u;
    options.raw = spec;
    CHECK_FALSE(inspect_audio_file_layout(bytes, options).error.empty());
    spec.channels = 1u;
    spec.sample_rate = 0u;
    options.raw = spec;
    CHECK_FALSE(inspect_audio_file_layout(bytes, options).error.empty());
}

TEST_CASE("default output carries no metadata text and names no path", "[audio][file-layout][redaction]") {
    Bytes bext(602u, 0u);
    const std::string speaker = "SPEAKER NAME 42";
    std::copy(speaker.begin(), speaker.end(), bext.begin());
    Bytes list{'I', 'N', 'F', 'O', 'I', 'N', 'A', 'M'};
    const std::string title = "Secret Title";
    append_u32(list, static_cast<std::uint32_t>(title.size()));
    for (char c : title) list.push_back(static_cast<std::uint8_t>(c));
    const auto bytes = make_wav_with_chunks(make_format(1u, 1u, 48'000u, 16u), two_frames_mono16(),
                                            {{"bext", bext}, {"LIST", list}});

    const auto redacted = inspect_audio_file_layout(bytes);
    CHECK(redacted.metadata_text.empty());
    CHECK(redacted.metadata_present.size() == 2u);
    LayoutJsonOptions json_options;
    json_options.input_name = "speech.wav";
    const auto json = audio_file_layout_to_json(redacted, json_options);
    CHECK_THAT(json, !Catch::Matchers::ContainsSubstring(speaker));
    CHECK_THAT(json, !Catch::Matchers::ContainsSubstring(title));
    CHECK_THAT(json, !Catch::Matchers::ContainsSubstring("\"text\""));
    CHECK_THAT(json, !Catch::Matchers::ContainsSubstring("/"));

    LayoutInspectOptions with_text;
    with_text.include_metadata_text = true;
    const auto lifted = inspect_audio_file_layout(bytes, with_text);
    REQUIRE(lifted.metadata_text.size() >= 2u);
    json_options.include_metadata_text = true;
    const auto full = audio_file_layout_to_json(lifted, json_options);
    CHECK_THAT(full, Catch::Matchers::ContainsSubstring(speaker));
    CHECK_THAT(full, Catch::Matchers::ContainsSubstring(title));
    CHECK_THAT(full, Catch::Matchers::ContainsSubstring("\"text\""));
}

TEST_CASE("inspect JSON carries its schema id and a fixed key set", "[audio][file-layout][schema]") {
    const auto report = inspect_audio_file_layout(make_wav(make_format(1u, 1u, 48'000u, 16u), two_frames_mono16()));
    LayoutJsonOptions options;
    options.input_name = "a.wav";
    const auto json = audio_file_layout_to_json(report, options);
    const auto parsed = choc::json::parse(json);
    CHECK(parsed["schema"].getString() == "pulp.audio.file_inspect.v1");
    CHECK(parsed["schema"].getString() == kAudioFileInspectJsonSchema);
    const std::set<std::string> expected{
        "schema", "input", "container", "container_detail", "profile", "conformance", "facts_confidence",
        "bytes_read", "stream", "decode", "exceeds_decode_limits", "regions", "regions_truncated",
        "warnings", "metadata"};
    CHECK(top_level_keys(json) == expected);
    CHECK(parsed["input"]["name"].getString() == "a.wav");
    CHECK(parsed["metadata"].hasObjectMember("present"));
    CHECK_FALSE(parsed["metadata"].hasObjectMember("text"));
    const auto region_keys = [&] {
        std::set<std::string> keys;
        const auto region = parsed["regions"][0];
        for (std::uint32_t i = 0; i < region.size(); ++i)
            keys.insert(std::string(region.getObjectMemberAt(i).name));
        return keys;
    }();
    CHECK(region_keys == std::set<std::string>{"offset", "declared_size", "actual_size", "id", "kind", "fields"});
}

#ifdef PULP_SOURCE_DIR
TEST_CASE("checked-in container fixtures walk end to end", "[audio][file-layout][fixtures]") {
    const auto root = pulp_test::fixture_root() / "test" / "fixtures" / "audio" / "inspect";
    auto inspect = [&](const char* name) {
        const auto path = root / name;
        INFO(path.string());
        REQUIRE(fs::exists(path));
        return inspect_audio_file_layout(path);
    };
    SECTION("AIFF") {
        const auto report = inspect("tone.aiff");
        CHECK(report.container == AudioContainer::aiff);
        CHECK(report.facts_confidence == FactsConfidence::parsed);
        REQUIRE(report.stream);
        CHECK(report.stream->sample_rate == 8000u);
        CHECK(report.stream->channels == 1u);
        CHECK(report.stream->bits_per_sample == 16u);
        CHECK(report.stream->frames == 800u);
        REQUIRE(report.decode);
        CHECK(report.decode->decodable);
        REQUIRE(find_region(report, "COMM"));
        REQUIRE(find_region(report, "SSND"));
        CHECK(report.is_standard());
    }
    SECTION("AIFF-C") {
        const auto report = inspect("tone.aifc");
        CHECK(report.container == AudioContainer::aifc);
        const auto* comm = find_region(report, "COMM");
        REQUIRE(comm);
        CHECK(std::any_of(comm->fields.begin(), comm->fields.end(),
                          [](const LayoutField& f) { return f.name == "compression_type"; }));
        REQUIRE(report.stream);
        CHECK(report.stream->frames == 800u);
        // afconvert names big-endian PCM 'twos'; the reader admits only NONE,
        // and the report says so instead of pretending otherwise.
        REQUIRE(report.decode);
        CHECK_FALSE(report.decode->decodable);
        CHECK_THAT(report.decode->reason, Catch::Matchers::ContainsSubstring("twos"));
        CHECK(report.is_standard());
    }
    SECTION("FLAC with SEEKTABLE") {
        const auto report = inspect("tone.flac");
        CHECK(report.container == AudioContainer::flac);
        REQUIRE(report.stream);
        CHECK(report.stream->frames == 800u);
        CHECK(report.stream->encoding == "flac");
        const auto* seektable = find_region(report, "SEEKTABLE");
        REQUIRE(seektable);
        CHECK(seektable->kind == LayoutRegionKind::metadata);
        REQUIRE(find_region(report, "STREAMINFO"));
        REQUIRE(find_region(report, "frames"));
        CHECK(report.bytes_read < 2048u);
    }
    SECTION("chained Ogg") {
        const auto report = inspect("chained.ogg");
        CHECK(report.container == AudioContainer::ogg);
        CHECK(report.facts_confidence == FactsConfidence::container_only);
        REQUIRE_FALSE(report.regions.empty());
        const auto& first = report.regions.front();
        const auto streams = std::find_if(first.fields.begin(), first.fields.end(),
                                          [](const LayoutField& f) { return f.name == "logical_streams"; });
        REQUIRE(streams != first.fields.end());
        CHECK(std::get<std::uint64_t>(streams->value) == 2u);
        const auto chained = std::find_if(first.fields.begin(), first.fields.end(),
                                          [](const LayoutField& f) { return f.name == "chained"; });
        REQUIRE(chained != first.fields.end());
        CHECK(std::get<bool>(chained->value));
        REQUIRE(report.stream);
        CHECK(report.stream->encoding == "vorbis");
        // A chained file has no single frame count to state.
        CHECK(report.stream->frames == 0u);
    }
    SECTION("MP3 with ID3v2, Xing and an APEv2 footer") {
        const auto report = inspect("tagged.mp3");
        CHECK(report.container == AudioContainer::mp3);
        CHECK(report.facts_confidence == FactsConfidence::container_only);
        REQUIRE(find_region(report, "ID3v2"));
        REQUIRE(find_region(report, "APEv2"));
        const auto* frames = find_region(report, "frames");
        REQUIRE(frames);
        CHECK(std::any_of(frames->fields.begin(), frames->fields.end(),
                          [](const LayoutField& f) { return f.name == "vbr_header"; }));
        CHECK(std::any_of(frames->fields.begin(), frames->fields.end(),
                          [](const LayoutField& f) { return f.name == "encoder_delay"; }));
        REQUIRE(report.stream);
        CHECK(report.stream->sample_rate == 8000u);
        CHECK(report.stream->frames > 0u);
        CHECK(report.metadata_text.empty());
        std::vector<std::string> present;
        for (const auto& entry : report.metadata_present) present.push_back(entry.id);
        CHECK(std::find(present.begin(), present.end(), "ID3v2") != present.end());
        CHECK(std::find(present.begin(), present.end(), "APEv2") != present.end());
    }
    SECTION("MP4 with co64 and a 64-bit mdat") {
        const auto report = inspect("co64.m4a");
        CHECK(report.container == AudioContainer::mp4);
        CHECK(report.facts_confidence == FactsConfidence::container_only);
        REQUIRE(find_region(report, "co64"));
        CHECK_FALSE(find_region(report, "stco"));
        const auto* mdat = find_region(report, "mdat");
        REQUIRE(mdat);
        CHECK(std::any_of(mdat->fields.begin(), mdat->fields.end(),
                          [](const LayoutField& f) { return f.name == "largesize"; }));
        const auto* stbl = find_region(report, "stbl");
        REQUIRE(stbl);
        const auto offsets = std::find_if(stbl->fields.begin(), stbl->fields.end(),
                                          [](const LayoutField& f) { return f.name == "chunk_offsets"; });
        REQUIRE(offsets != stbl->fields.end());
        CHECK(std::get<std::string>(offsets->value) == "co64");
        REQUIRE(report.stream);
        CHECK(report.stream->sample_rate == 8000u);
        CHECK(report.stream->channels == 1u);
        CHECK_THAT(report.stream->encoding, Catch::Matchers::ContainsSubstring("aac"));
    }
}
#endif
