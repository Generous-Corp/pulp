// test_wav_metadata.cpp — round-trip tests for BWAV / iXML / ASWG / ACID
// metadata. Covers item 6.11 of the 2026-05-24 macOS plugin authoring
// plan.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/wav_metadata.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

std::filesystem::path scratch_file(const char* tag) {
    auto base = std::filesystem::temp_directory_path();
    auto p = base / (std::string("pulp-wav-metadata-") + tag + "-"
                     + std::to_string(static_cast<unsigned long long>(std::time(nullptr)))
                     + "-"
                     + std::to_string(static_cast<unsigned long long>(std::rand()))
                     + ".wav");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p;
}

BwavMetadata make_bwav_fixture() {
    BwavMetadata b;
    b.description = "Pulp test BWAV — round-trip fixture";
    b.originator = "PulpRecorderSim";
    b.originator_reference = "PR-2026-0524-0001";
    b.origination_date = "2026-05-24";
    b.origination_time = "14:25:01";
    b.time_reference = 123'456'789ULL * 2ULL; // > 32 bits to exercise high u32
    b.version = 2;
    for (size_t i = 0; i < b.umid.size(); ++i)
        b.umid[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    b.loudness_value = -2300;       // -23.00 LUFS in EBU units
    b.loudness_range = 950;         // 9.50 LU
    b.max_true_peak_level = -150;   // -1.50 dBTP
    b.max_momentary_loudness = -1850;
    b.max_short_term_loudness = -2100;
    b.coding_history = "A=PCM,F=48000,W=24,M=mono,T=Pulp\r\n";
    return b;
}

AcidMetadata make_acid_fixture() {
    AcidMetadata a;
    a.flags = 0x02 | 0x04; // root note set + stretch
    a.root_note = 69;      // A4
    a.reserved1 = 0x8000;
    a.reserved2 = 0.0f;
    a.num_beats = 16;
    a.meter_denominator = 8;
    a.meter_numerator = 7;     // 7/8
    a.tempo_bpm = 124.25f;
    return a;
}

bool bwav_eq(const BwavMetadata& a, const BwavMetadata& b) {
    return a.description == b.description
        && a.originator == b.originator
        && a.originator_reference == b.originator_reference
        && a.origination_date == b.origination_date
        && a.origination_time == b.origination_time
        && a.time_reference == b.time_reference
        && a.version == b.version
        && a.umid == b.umid
        && a.loudness_value == b.loudness_value
        && a.loudness_range == b.loudness_range
        && a.max_true_peak_level == b.max_true_peak_level
        && a.max_momentary_loudness == b.max_momentary_loudness
        && a.max_short_term_loudness == b.max_short_term_loudness
        && a.coding_history == b.coding_history;
}

} // namespace

TEST_CASE("BWAV bext chunk round-trips through encode/decode", "[audio][wav][metadata][bwav][issue-6_11]") {
    auto src = make_bwav_fixture();
    auto bytes = encode_bext_chunk(src);
    // Fixed header (602) + coding_history length.
    REQUIRE(bytes.size() == 602 + src.coding_history.size());

    auto rt = decode_bext_chunk(bytes.data(), bytes.size());
    REQUIRE(rt.has_value());
    REQUIRE(bwav_eq(src, *rt));
}

TEST_CASE("BWAV bext truncates over-long fixed strings without UB", "[audio][wav][metadata][bwav][issue-6_11]") {
    BwavMetadata src;
    src.description = std::string(400, 'x'); // overflow 256-byte slot
    src.originator = std::string(60, 'y');   // overflow 32-byte slot
    src.origination_date = "2026-05-24extra"; // overflow 10-byte slot

    auto bytes = encode_bext_chunk(src);
    auto rt = decode_bext_chunk(bytes.data(), bytes.size());
    REQUIRE(rt.has_value());
    CHECK(rt->description.size() == 256);
    CHECK(rt->originator.size() == 32);
    CHECK(rt->origination_date == "2026-05-24");
}

TEST_CASE("BWAV decode rejects undersized payload", "[audio][wav][metadata][bwav][issue-6_11]") {
    std::vector<uint8_t> garbage(64, 0xAB);
    auto rt = decode_bext_chunk(garbage.data(), garbage.size());
    REQUIRE_FALSE(rt.has_value());
}

TEST_CASE("ACID chunk round-trips through encode/decode", "[audio][wav][metadata][acid][issue-6_11]") {
    auto src = make_acid_fixture();
    auto bytes = encode_acid_chunk(src);
    REQUIRE(bytes.size() == 24);

    auto rt = decode_acid_chunk(bytes.data(), bytes.size());
    REQUIRE(rt.has_value());
    CHECK(rt->flags == src.flags);
    CHECK(rt->root_note == src.root_note);
    CHECK(rt->reserved1 == src.reserved1);
    CHECK_THAT(rt->reserved2, WithinAbs(src.reserved2, 1e-6f));
    CHECK(rt->num_beats == src.num_beats);
    CHECK(rt->meter_denominator == src.meter_denominator);
    CHECK(rt->meter_numerator == src.meter_numerator);
    CHECK_THAT(rt->tempo_bpm, WithinAbs(src.tempo_bpm, 1e-4f));
}

TEST_CASE("ACID decode rejects undersized payload", "[audio][wav][metadata][acid][issue-6_11]") {
    std::vector<uint8_t> tiny(12, 0);
    auto rt = decode_acid_chunk(tiny.data(), tiny.size());
    REQUIRE_FALSE(rt.has_value());
}

TEST_CASE("Full WAV metadata file round-trip with all four chunk kinds", "[audio][wav][metadata][issue-6_11]") {
    auto path = scratch_file("full");
    WavMetadata src;
    src.bwav = make_bwav_fixture();
    src.acid = make_acid_fixture();
    src.ixml = "<?xml version=\"1.0\"?><BWFXML><PROJECT>Pulp Sandbox</PROJECT>"
               "<SCENE>S01</SCENE><TAKE>03</TAKE></BWFXML>";
    src.axml = "<?xml version=\"1.0\"?><ebuCoreMain xmlns=\"urn:ebu:metadata-schema:ebuCore_2014\">"
               "<title>ASWG sample</title></ebuCoreMain>";

    REQUIRE(write_wav_metadata(path.string(), src, 48000, 1, 16));
    REQUIRE(std::filesystem::exists(path));

    auto rt = read_wav_metadata(path.string());
    REQUIRE(rt.has_value());
    REQUIRE(rt->bwav.has_value());
    REQUIRE(rt->acid.has_value());
    REQUIRE(rt->ixml.has_value());
    REQUIRE(rt->axml.has_value());

    CHECK(bwav_eq(*src.bwav, *rt->bwav));
    CHECK(rt->acid->tempo_bpm == src.acid->tempo_bpm);
    CHECK(rt->acid->num_beats == src.acid->num_beats);
    CHECK(rt->acid->meter_numerator == 7);
    CHECK(rt->acid->meter_denominator == 8);
    CHECK(*rt->ixml == *src.ixml);
    CHECK(*rt->axml == *src.axml);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("read_wav_metadata returns empty struct for WAV with no metadata", "[audio][wav][metadata][issue-6_11]") {
    auto path = scratch_file("bare");
    WavMetadata empty; // no bwav/acid/ixml/axml
    REQUIRE(write_wav_metadata(path.string(), empty, 44100, 2, 24));

    auto rt = read_wav_metadata(path.string());
    REQUIRE(rt.has_value());
    CHECK_FALSE(rt->bwav.has_value());
    CHECK_FALSE(rt->acid.has_value());
    CHECK_FALSE(rt->ixml.has_value());
    CHECK_FALSE(rt->axml.has_value());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("read_wav_metadata fails cleanly on non-existent path", "[audio][wav][metadata][issue-6_11]") {
    auto missing = std::filesystem::temp_directory_path()
                 / "pulp-wav-metadata-this-does-not-exist-xyz.wav";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    auto rt = read_wav_metadata(missing.string());
    REQUIRE_FALSE(rt.has_value());
}

TEST_CASE("read_wav_metadata fails on non-RIFF data", "[audio][wav][metadata][issue-6_11]") {
    auto path = scratch_file("notriff");
    {
        std::ofstream out(path, std::ios::binary);
        out << "This is not a WAV file at all, just text.";
    }
    auto rt = read_wav_metadata(path.string());
    REQUIRE_FALSE(rt.has_value());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("replace_metadata_in_file preserves fmt/data and rewrites chunks", "[audio][wav][metadata][issue-6_11]") {
    auto path = scratch_file("inplace");

    WavMetadata initial;
    initial.bwav = make_bwav_fixture();
    initial.bwav->description = "INITIAL description";
    REQUIRE(write_wav_metadata(path.string(), initial, 44100, 2, 24));

    auto initial_size = std::filesystem::file_size(path);
    REQUIRE(initial_size > 0);

    WavMetadata replacement;
    replacement.bwav = make_bwav_fixture();
    replacement.bwav->description = "REPLACED description";
    replacement.acid = make_acid_fixture();
    replacement.acid->tempo_bpm = 90.0f;
    replacement.ixml = "<?xml?><added/>";

    REQUIRE(replace_metadata_in_file(path.string(), replacement));

    auto rt = read_wav_metadata(path.string());
    REQUIRE(rt.has_value());
    REQUIRE(rt->bwav.has_value());
    REQUIRE(rt->acid.has_value());
    REQUIRE(rt->ixml.has_value());
    CHECK(rt->bwav->description == "REPLACED description");
    CHECK(rt->acid->tempo_bpm == 90.0f);
    CHECK(*rt->ixml == "<?xml?><added/>");

    // Replacing again with empty metadata should strip everything but
    // leave fmt + data intact (file should still parse as a WAV).
    WavMetadata stripped;
    REQUIRE(replace_metadata_in_file(path.string(), stripped));
    auto bare = read_wav_metadata(path.string());
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->bwav.has_value());
    CHECK_FALSE(bare->acid.has_value());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Unknown chunks survive a read+write round-trip", "[audio][wav][metadata][issue-6_11]") {
    auto path = scratch_file("unknown");

    WavMetadata src;
    WavMetadata::UnknownChunk u;
    u.id = {{'L', 'I', 'S', 'T'}};
    // Synthesize a tiny INFO LIST payload — "INFOIART\0\0\0\x05Pulp\0".
    const uint8_t list_payload[] = {
        'I','N','F','O',
        'I','A','R','T', 0x05,0x00,0x00,0x00, 'P','u','l','p','\0',
    };
    u.data.assign(std::begin(list_payload), std::end(list_payload));
    src.unknown_chunks.push_back(u);

    REQUIRE(write_wav_metadata(path.string(), src, 48000, 1, 16));
    auto rt = read_wav_metadata(path.string());
    REQUIRE(rt.has_value());
    REQUIRE(rt->unknown_chunks.size() == 1);
    CHECK(rt->unknown_chunks[0].id == u.id);
    CHECK(rt->unknown_chunks[0].data == u.data);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
