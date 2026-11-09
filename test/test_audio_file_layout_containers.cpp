// Layout report over the non-WAV containers: the checked-in AIFF, AIFF-C,
// FLAC, Ogg, MP3 and MP4 fixtures, and hand-built bytes for the facts each
// container walk states.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/audio/audio_file_layout.hpp>
#include <pulp/audio/audio_file_layout_json.hpp>
#include <pulp/audio/format_registry.hpp>

#include <choc/text/choc_JSON.h>

#include "support/container_fixture_builders.hpp"
#include "support/fixture_root.hpp"
#include "support/wav_fixture_builders.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace pulp_test::wav;
using namespace pulp_test::container;
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

std::string to_json(const AudioFileLayoutReport& report, bool include_text = false) {
    LayoutJsonOptions options;
    options.input_name = "x";
    options.include_metadata_text = include_text;
    return audio_file_layout_to_json(report, options);
}

} // namespace

#ifdef PULP_SOURCE_DIR
TEST_CASE("afconvert's AIFF-C twos fixture decodes to the same samples as its AIFF twin",
          "[audio][file-layout][fixtures][aifc-pcm]") {
    const auto root = pulp_test::fixture_root() / "test" / "fixtures" / "audio" / "inspect";
    const auto aiff_path = root / "tone.aiff";
    const auto aifc_path = root / "tone.aifc";
    REQUIRE(fs::exists(aiff_path));
    REQUIRE(fs::exists(aifc_path));

    auto& registry = FormatRegistry::instance();
    const auto aiff = registry.read(aiff_path.string());
    const auto aifc = registry.read(aifc_path.string());
    REQUIRE(aiff);
    REQUIRE(aifc);
    CHECK(aiff->sample_rate == 8000u);
    CHECK(aifc->sample_rate == 8000u);
    REQUIRE(aiff->num_frames() == 800u);
    REQUIRE(aifc->num_frames() == 800u);
    // Both files were written from one source by afconvert at BEI16; the only
    // difference is the AIFC envelope and its 'twos' label, so the decoded
    // samples must be identical floats.
    CHECK(aifc->channels == aiff->channels);
    // The tone is not silence: a decoder that zero-filled both would also pass
    // the equality above.
    CHECK(std::any_of(aiff->channels[0].begin(), aiff->channels[0].end(),
                      [](float sample) { return sample != 0.0f; }));
}
#endif

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
        // afconvert names big-endian PCM 'twos'; the reader decodes it as the
        // same stream AIFF carries, and the verdict follows the reader.
        REQUIRE(report.decode);
        CHECK(report.decode->decodable);
        CHECK(report.decode->reason.empty());
        CHECK(report.stream->bytes_per_frame == 2u);
        CHECK(report.is_standard());
        CHECK(report.stream->encoding == "pcm");
        CHECK(report.metadata_text.empty());
        LayoutJsonOptions json_options;
        json_options.input_name = "tone.aifc";
        CHECK_THAT(audio_file_layout_to_json(report, json_options),
                   !Catch::Matchers::ContainsSubstring("Linear PCM"));
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
        const auto streams =
            std::find_if(first.fields.begin(), first.fields.end(),
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
        for (const auto* region : report.metadata_regions())
            present.push_back(region->id);
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
        const auto offsets =
            std::find_if(stbl->fields.begin(), stbl->fields.end(),
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

TEST_CASE("file text in fields stays out of the default report",
          "[audio][file-layout][redaction]") {
    SECTION("FLAC PICTURE mime type") {
        Bytes picture;
        put_be32(picture, 3u);
        const std::string mime = "image/png";
        put_be32(picture, static_cast<std::uint32_t>(mime.size()));
        put_text(picture, mime);
        put_be32(picture, 0u);
        put_be32(picture, 16u);
        put_be32(picture, 16u);
        put_be32(picture, 24u);
        put_be32(picture, 0u);
        put_be32(picture, 0u);
        Bytes flac{'f', 'L', 'a', 'C', 0u, 0u, 0u, 34u};
        flac.resize(8u + 34u, 0u);
        flac[8] = 0x10u;
        flac[10] = 0x10u;
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(48000u) << 44u) | (15ull << 36u) | 800u;
        for (unsigned i = 0; i < 8u; ++i)
            flac[18u + i] = static_cast<std::uint8_t>(packed >> (56u - 8u * i));
        flac.push_back(0x86u);
        flac.push_back(0u);
        flac.push_back(0u);
        flac.push_back(static_cast<std::uint8_t>(picture.size()));
        flac.insert(flac.end(), picture.begin(), picture.end());
        const auto quiet = inspect_audio_file_layout(flac);
        CHECK_THAT(to_json(quiet), !Catch::Matchers::ContainsSubstring(mime));
        const auto* region = find_region(quiet, "PICTURE");
        REQUIRE(region);
        CHECK(std::any_of(region->fields.begin(), region->fields.end(),
                          [](const LayoutField& f) { return f.name == "mime_length"; }));
        LayoutInspectOptions options;
        options.include_metadata_text = true;
        const auto lifted = inspect_audio_file_layout(flac, options);
        CHECK_THAT(to_json(lifted, true), Catch::Matchers::ContainsSubstring(mime));
    }
}

TEST_CASE("Ogg facts: a -1 granule is unknown and FLAC-in-Ogg comments are read",
          "[audio][file-layout][ogg]") {
    SECTION("Opus stream ending on a page with no finished packet") {
        Bytes head;
        put_text(head, "OpusHead");
        head.push_back(1u);
        head.push_back(2u);
        append_u16(head, 312u);
        append_u32(head, 48000u);
        append_u16(head, 0u);
        head.push_back(0u);
        Bytes tags;
        put_text(tags, "OpusTags");
        tags = cat({tags, vorbis_comment_body("test", {"TITLE=t"})});
        const auto bytes = cat({ogg_page(0x02u, 0u, 7u, 0u, head), ogg_page(0u, 0u, 7u, 1u, tags),
                                ogg_page(0x04u, 0xffffffffffffffffull, 7u, 2u, Bytes(10u, 0u))});
        const auto report = inspect_audio_file_layout(bytes);
        REQUIRE(report.stream);
        CHECK(report.stream->encoding == "opus");
        CHECK(report.stream->channels == 2u);
        CHECK(report.stream->frames == 0u);
        CHECK(report.stream->duration_seconds == 0.0);
        const auto parsed = choc::json::parse(to_json(report));
        CHECK(parsed["stream"]["frames"].getInt64() == 0);
    }
    SECTION("FLAC-in-Ogg comment header behind its block header") {
        Bytes ident{0x7fu};
        put_text(ident, "FLAC");
        ident.push_back(1u);
        ident.push_back(0u);
        put_be16(ident, 1u);
        put_text(ident, "fLaC");
        ident.insert(ident.end(), {0x80u, 0u, 0u, 34u});
        const auto info_start = ident.size();
        ident.resize(info_start + 34u, 0u);
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(44100u) << 44u) | (15ull << 36u) | 100u;
        for (unsigned i = 0; i < 8u; ++i)
            ident[info_start + 10u + i] = static_cast<std::uint8_t>(packed >> (56u - 8u * i));
        const auto comment_body = vorbis_comment_body("ref", {"ARTIST=a", "TITLE=b"});
        Bytes comment{0x84u, 0u, 0u, static_cast<std::uint8_t>(comment_body.size())};
        comment.insert(comment.end(), comment_body.begin(), comment_body.end());
        const auto bytes =
            cat({ogg_page(0x02u, 0u, 9u, 0u, ident), ogg_page(0u, 0u, 9u, 1u, comment),
                 ogg_page(0x04u, 100u, 9u, 2u, Bytes(8u, 0u))});
        LayoutInspectOptions options;
        options.include_metadata_text = true;
        const auto report = inspect_audio_file_layout(bytes, options);
        REQUIRE(report.stream);
        CHECK(report.stream->encoding == "flac");
        CHECK(report.stream->sample_rate == 44100u);
        CHECK(report.stream->frames == 100u);
        REQUIRE(report.regions.size() >= 2u);
        const auto& comment_region = report.regions[1];
        const auto count =
            std::find_if(comment_region.fields.begin(), comment_region.fields.end(),
                         [](const LayoutField& f) { return f.name == "comment_count"; });
        REQUIRE(count != comment_region.fields.end());
        CHECK(std::get<std::uint64_t>(count->value) == 2u);
        CHECK(std::any_of(
            report.metadata_text.begin(), report.metadata_text.end(),
            [](const LayoutMetadataText& t) { return t.key == "ARTIST" && t.text == "a"; }));
    }
}

TEST_CASE("MP4 facts come from the media timescale and children stay inside their parent",
          "[audio][file-layout][mp4]") {
    Bytes ftyp;
    put_text(ftyp, "M4A ");
    put_be32(ftyp, 0u);
    put_text(ftyp, "isom");
    Bytes mdhd(12u, 0u);
    put_be32(mdhd, 1000u); // timescale
    put_be32(mdhd, 2000u); // duration: 2.000 s
    put_be32(mdhd, 0u);
    Bytes hdlr(8u, 0u);
    put_text(hdlr, "soun");
    hdlr.resize(hdlr.size() + 13u, 0u);
    Bytes entry;
    put_be32(entry, 36u);
    put_text(entry, "mp4a");
    entry.resize(entry.size() + 16u, 0u);
    put_be16(entry, 1u);  // channels
    put_be16(entry, 16u); // sample size
    put_be32(entry, 0u);
    put_be32(entry, 8000u << 16u);
    Bytes stsd(4u, 0u);
    put_be32(stsd, 1u);
    stsd.insert(stsd.end(), entry.begin(), entry.end());
    Bytes stts(4u, 0u);
    put_be32(stts, 1u);
    put_be32(stts, 10u);
    put_be32(stts, 200u); // 10 x 200 = 2000 media units
    auto stts_box = box("stts", stts);
    SECTION("duration from mdhd, frames only when the timescale is the sample rate") {
        const auto bytes =
            cat({box("ftyp", ftyp),
                 box("moov",
                     box("trak", box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr),
                                                  box("minf", box("stbl", cat({box("stsd", stsd),
                                                                               stts_box})))}))))});
        const auto report = inspect_audio_file_layout(bytes);
        REQUIRE(report.stream);
        CHECK(report.stream->sample_rate == 8000u);
        CHECK(report.stream->frames == 0u);
        CHECK(report.stream->duration_seconds == 2.0);
        CHECK(report.is_standard());
    }
    SECTION("a child box that overruns its parent is clamped and reported") {
        // Claim 400 bytes for stts inside a stbl that holds far fewer.
        auto oversized = stts_box;
        oversized[0] = 0u;
        oversized[1] = 0u;
        oversized[2] = 0x01u;
        oversized[3] = 0x90u;
        const auto bytes =
            cat({box("ftyp", ftyp),
                 box("moov",
                     box("trak", box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr),
                                                  box("minf", box("stbl", cat({box("stsd", stsd),
                                                                               oversized})))}))))});
        const auto report = inspect_audio_file_layout(bytes);
        CHECK(has_warning(report, LayoutWarningKind::chunk_past_eof));
        const auto* region = find_region(report, "stts");
        REQUIRE(region);
        CHECK(region->declared_size == 392u);
        CHECK(region->actual_size == stts.size());
        const auto parsed = choc::json::parse(to_json(report));
        CHECK(parsed["warnings"].size() == 1u);
    }
}

TEST_CASE("ID3v2 unsynchronisation is undone before frames are read", "[audio][file-layout][mp3]") {
    // A TIT2 frame whose text holds 0xFF bytes; under unsynchronisation each
    // is followed by an inserted 0x00 that the reader must remove.
    const std::string title = "A\xff"
                              "B";
    Bytes frame;
    put_text(frame, "TIT2");
    put_be32(frame, static_cast<std::uint32_t>(1u + title.size()));
    put_be16(frame, 0u);
    frame.push_back(0u); // ISO-8859-1
    put_text(frame, title);
    Bytes unsynced;
    for (const auto byte : frame) {
        unsynced.push_back(byte);
        if (byte == 0xffu)
            unsynced.push_back(0u);
    }
    Bytes tag;
    put_text(tag, "ID3");
    tag.push_back(3u);
    tag.push_back(0u);
    tag.push_back(0x80u);
    const auto size = static_cast<std::uint32_t>(unsynced.size());
    tag.push_back(static_cast<std::uint8_t>((size >> 21u) & 0x7fu));
    tag.push_back(static_cast<std::uint8_t>((size >> 14u) & 0x7fu));
    tag.push_back(static_cast<std::uint8_t>((size >> 7u) & 0x7fu));
    tag.push_back(static_cast<std::uint8_t>(size & 0x7fu));
    tag.insert(tag.end(), unsynced.begin(), unsynced.end());
    // One valid MPEG-1 layer III frame header so the stream is recognised.
    tag.insert(tag.end(), {0xffu, 0xfbu, 0x90u, 0xc0u});
    tag.resize(tag.size() + 413u, 0u);
    LayoutInspectOptions options;
    options.include_metadata_text = true;
    const auto report = inspect_audio_file_layout(tag, options);
    CHECK(report.container == AudioContainer::mp3);
    const auto* id3 = find_region(report, "ID3v2");
    REQUIRE(id3);
    const auto frames = std::find_if(id3->fields.begin(), id3->fields.end(),
                                     [](const LayoutField& f) { return f.name == "frames"; });
    REQUIRE(frames != id3->fields.end());
    CHECK(std::get<std::uint64_t>(frames->value) == 1u);
    const auto text = std::find_if(report.metadata_text.begin(), report.metadata_text.end(),
                                   [](const LayoutMetadataText& t) { return t.key == "TIT2"; });
    REQUIRE(text != report.metadata_text.end());
    CHECK(text->text == title);
}

TEST_CASE("an AIFF COMM rate that is not finite is noted, not emitted",
          "[audio][file-layout][aiff]") {
    Bytes comm;
    put_be16(comm, 1u);
    put_be32(comm, 2u);
    put_be16(comm, 16u);
    comm.insert(comm.end(),
                {0x7fu, 0xffu, 0x80u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}); // exponent all ones: infinity
    Bytes body;
    put_text(body, "AIFF");
    body = cat({body, iff_chunk("COMM", comm), iff_chunk("SSND", Bytes(12u, 0u))});
    Bytes form;
    put_text(form, "FORM");
    put_be32(form, static_cast<std::uint32_t>(body.size()));
    form.insert(form.end(), body.begin(), body.end());
    const auto report = inspect_audio_file_layout(form);
    const auto* region = find_region(report, "COMM");
    REQUIRE(region);
    CHECK(std::none_of(region->fields.begin(), region->fields.end(),
                       [](const LayoutField& f) { return f.name == "sample_rate"; }));
    CHECK_THAT(region->note, Catch::Matchers::ContainsSubstring("not a finite number"));
    REQUIRE(report.decode);
    CHECK_FALSE(report.decode->decodable);
    CHECK_NOTHROW(choc::json::parse(to_json(report)));
}
