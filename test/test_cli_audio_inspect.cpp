// Shell-out tests for `pulp audio inspect` and the `validate summarize --json`
// schema companion. They launch the built CLI, assert exit codes and stderr,
// and parse the JSON documents the verbs print.

#include "test_cli_shellout_helpers.hpp"

#include "support/wav_fixture_builders.hpp"

#include <choc/text/choc_JSON.h>

#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>

using namespace pulp_test_cli;
using namespace pulp_test::wav;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& name) {
    return fs::temp_directory_path() /
           ("pulp-cli-audio-inspect-" + std::to_string(::getpid()) + "-" + name);
}

fs::path write_bytes(const std::string& name, const Bytes& bytes) {
    const auto path = temp_path(name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
    return path;
}

Bytes valid_wav() {
    Bytes data;
    append_sample(data, std::int16_t{0});
    append_sample(data, std::int16_t{1000});
    return make_wav(make_format(1u, 1u, 48'000u, 16u), data);
}

std::set<std::string> keys_of(const choc::value::ValueView& object) {
    std::set<std::string> keys;
    for (std::uint32_t i = 0; i < object.size(); ++i)
        keys.insert(std::string(object.getObjectMemberAt(i).name));
    return keys;
}

} // namespace

TEST_CASE("audio inspect --raw refuses a partial description", "[cli][shellout][audio-inspect]") {
    if (!binary_exists())
        SKIP("pulp not built");
    const auto path = write_bytes("raw.pcm", Bytes(10u, 0u));

    auto r = run_pulp({"audio", "inspect", path.string(), "--raw", "--format", "s16"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("--raw requires every one of"));
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring(
                                    "missing --channels, --rate, --endian, --offset"));
    CHECK(r.stdout_output.empty());

    // A raw field without --raw is refused too, so nothing is ever inferred.
    r = run_pulp({"audio", "inspect", path.string(), "--channels", "2"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("apply only with --raw"));

    r = run_pulp({"audio", "inspect", path.string(), "--raw", "--format", "s16", "--channels", "0",
                  "--rate", "8000", "--endian", "little", "--offset", "0"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("--channels must be between 1"));

    r = run_pulp({"audio", "inspect", path.string(), "--raw", "--format", "s16", "--channels", "1",
                  "--rate", "0", "--endian", "little", "--offset", "0"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("--rate must be between 1"));

    r = run_pulp({"audio", "inspect", path.string(), "--raw", "--format", "s16", "--channels", "1",
                  "--rate", "8000", "--endian", "middle", "--offset", "0"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output,
               Catch::Matchers::ContainsSubstring("--endian must be little or big"));

    r = run_pulp({"audio", "inspect", path.string(), "--raw", "--format", "s16", "--channels", "1",
                  "--rate", "8000", "--endian", "little", "--offset", "64", "--json"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("past the end of the file"));
    fs::remove(path);
}

TEST_CASE("audio inspect --raw reports the bytes after the last whole frame",
          "[cli][shellout][audio-inspect]") {
    if (!binary_exists())
        SKIP("pulp not built");
    const auto path = write_bytes("raw-tail.pcm", Bytes(11u, 0u));
    const auto r =
        run_pulp({"audio", "inspect", path.string(), "--raw", "--format", "s16", "--channels", "1",
                  "--rate", "8000", "--endian", "little", "--offset", "2", "--json"});
    REQUIRE(r.exit_code == 0);
    const auto doc = choc::json::parse(r.stdout_output);
    CHECK(doc["container"].getString() == "raw");
    CHECK(doc["stream"]["frames"].getInt64() == 4);
    CHECK(doc["truncated_tail_bytes"].getInt64() == 1);
    CHECK(doc["regions"][1]["offset"].getInt64() == 2);
    fs::remove(path);
}

TEST_CASE("audio inspect and validate summarize JSON carry their schema ids",
          "[cli][shellout][audio-inspect][schema]") {
    if (!binary_exists())
        SKIP("pulp not built");
    const auto path = write_bytes("schema.wav", valid_wav());

    const auto inspect = run_pulp({"audio", "inspect", path.string(), "--json"});
    REQUIRE(inspect.exit_code == 0);
    const auto doc = choc::json::parse(inspect.stdout_output);
    CHECK(doc["schema"].getString() == "pulp.audio.file_inspect.v1");
    CHECK(keys_of(doc) == std::set<std::string>{"schema", "input", "container", "container_detail",
                                                "profile", "conformance", "facts_confidence",
                                                "bytes_read", "stream", "decode",
                                                "exceeds_decode_limits", "regions",
                                                "regions_truncated", "warnings", "metadata"});
    CHECK(doc["conformance"].getString() == "standard");
    CHECK(doc["profile"].getString() == "pcm44");

    const auto summarize = run_pulp({"audio", "validate", "summarize", path.string(), "--json"});
    REQUIRE(summarize.exit_code == 0);
    const auto metrics = choc::json::parse(summarize.stdout_output);
    CHECK(metrics["schema"].getString() == "pulp.audio.summarize.v1");
    CHECK(keys_of(metrics) == std::set<std::string>{"schema", "schema_version", "scenario",
                                                    "sample_rate", "frames", "num_channels",
                                                    "channels"});
    fs::remove(path);
}

TEST_CASE("audio inspect and summarize name inputs by basename unless asked",
          "[cli][shellout][audio-inspect][redaction]") {
    if (!binary_exists())
        SKIP("pulp not built");
    Bytes bext(602u, 0u);
    const std::string speaker = "Speaker Identity";
    std::copy(speaker.begin(), speaker.end(), bext.begin());
    Bytes data;
    append_sample(data, std::int16_t{0});
    const auto path =
        write_bytes("speech.wav", make_wav_with_chunks(make_format(1u, 1u, 48'000u, 16u), data,
                                                       {{"bext", bext}}));
    const auto parent = path.parent_path().string();

    auto r = run_pulp({"audio", "inspect", path.string(), "--json"});
    REQUIRE(r.exit_code == 0);
    auto doc = choc::json::parse(r.stdout_output);
    CHECK(doc["input"]["name"].getString() == path.filename().string());
    CHECK_THAT(r.stdout_output, !Catch::Matchers::ContainsSubstring(parent));
    CHECK_THAT(r.stdout_output, !Catch::Matchers::ContainsSubstring(speaker));
    CHECK_FALSE(doc["metadata"].hasObjectMember("text"));
    CHECK(doc["metadata"]["present"][0]["id"].getString() == "bext");

    r = run_pulp({"audio", "inspect", path.string(), "--json", "--include-metadata-text",
                  "--absolute-paths"});
    REQUIRE(r.exit_code == 0);
    doc = choc::json::parse(r.stdout_output);
    CHECK(doc["input"]["name"].getString() == path.string());
    CHECK(doc["metadata"].hasObjectMember("text"));
    CHECK_THAT(r.stdout_output, Catch::Matchers::ContainsSubstring(speaker));

    r = run_pulp({"audio", "validate", "summarize", path.string(), "--json"});
    REQUIRE(r.exit_code == 0);
    CHECK(choc::json::parse(r.stdout_output)["scenario"].getString() == path.filename().string());
    r = run_pulp({"audio", "validate", "summarize", path.string(), "--json", "--absolute-paths"});
    REQUIRE(r.exit_code == 0);
    CHECK(choc::json::parse(r.stdout_output)["scenario"].getString() == path.string());
    fs::remove(path);
}

TEST_CASE("a header past the decode limit is a fact for inspect and a named limit for summarize",
          "[cli][shellout][audio-inspect][limits]") {
    if (!binary_exists())
        SKIP("pulp not built");
    // 100,000,001 mono 16-bit frames: a sparse file holds the declaration
    // without the 200 MB of samples.
    constexpr std::uint64_t kFrames = 100'000'001u;
    auto header = make_wav(make_format(1u, 1u, 48'000u, 16u), Bytes{});
    set_u32(header, 40u, static_cast<std::uint32_t>(kFrames * 2u));
    set_u32(header, 4u, static_cast<std::uint32_t>(kFrames * 2u + 36u));
    const auto path = write_bytes("over-limit.wav", header);
    fs::resize_file(path, kFrames * 2u + 44u);

    auto r = run_pulp({"audio", "inspect", path.string(), "--json"});
    REQUIRE(r.exit_code == 0);
    const auto doc = choc::json::parse(r.stdout_output);
    CHECK(doc["stream"]["frames"].getInt64() == static_cast<std::int64_t>(kFrames));
    CHECK(doc["decode"]["decodable"].getBool());
    CHECK(doc["exceeds_decode_limits"]["exceeded"].getBool());
    CHECK(doc["exceeds_decode_limits"]["exceeded_by"][0].getString() == "max_frames");
    CHECK(doc["bytes_read"].getInt64() < 1024);

    fs::remove(path);

    // `summarize` decodes FLAC through the format registry, whose ceiling is
    // 2^30 frames; a STREAMINFO one frame past it must be named as a limit,
    // not as corruption. The header alone is enough to trip it.
    constexpr std::uint64_t kRegistryFrames = (1ull << 30u) + 1u;
    Bytes flac{'f', 'L', 'a', 'C', 0x80u, 0u, 0u, 34u};
    flac.resize(8u + 34u, 0u);
    flac[8] = 0x10u;
    flac[10] = 0x10u; // block size 4096
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(48000u) << 44u) | (15ull << 36u) | kRegistryFrames;
    for (unsigned i = 0; i < 8u; ++i)
        flac[18u + i] = static_cast<std::uint8_t>(packed >> (56u - 8u * i));
    const auto over_path = write_bytes("over-registry-limit.flac", flac);
    r = run_pulp({"audio", "validate", "summarize", over_path.string(), "--json"});
    CHECK(r.exit_code == 1);
    CHECK_THAT(r.stderr_output,
               Catch::Matchers::ContainsSubstring("exceeds the in-memory decode limit"));
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("max_frames=1073741824"));
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("pulp audio inspect"));
    fs::remove(over_path);
}

TEST_CASE("audio inspect answers from bytes, not the file extension",
          "[cli][shellout][audio-inspect]") {
    if (!binary_exists())
        SKIP("pulp not built");
    Bytes flac{'f', 'L', 'a', 'C', 0x80u, 0u, 0u, 34u};
    flac.resize(8u + 34u, 0u);
    flac[8] = 0x10u;
    flac[9] = 0x00u;
    flac[10] = 0x10u;
    flac[11] = 0x00u; // block size 4096
    // 44100 Hz (20 bits), 1 channel (3 bits), 16 bits (5 bits), 0 samples
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(44100u) << 44u) | (0ull << 41u) | (15ull << 36u);
    for (unsigned i = 0; i < 8u; ++i)
        flac[18u + i] = static_cast<std::uint8_t>(packed >> (56u - 8u * i));
    const auto path = write_bytes("lying.wav", flac);
    const auto r = run_pulp({"audio", "inspect", path.string(), "--json"});
    REQUIRE(r.exit_code == 0);
    const auto doc = choc::json::parse(r.stdout_output);
    CHECK(doc["container"].getString() == "flac");
    CHECK(doc["stream"]["sample_rate"].getInt64() == 44100);
    fs::remove(path);
}

TEST_CASE("audio inspect exits 2 on an unrecognized container and still reports",
          "[cli][shellout][audio-inspect]") {
    if (!binary_exists())
        SKIP("pulp not built");
    const std::string text = "this is not audio\n";
    const auto path = write_bytes("notes.txt", Bytes(text.begin(), text.end()));
    const auto r = run_pulp({"audio", "inspect", path.string(), "--json"});
    CHECK(r.exit_code == 2);
    const auto doc = choc::json::parse(r.stdout_output);
    CHECK(doc["container"].getString() == "unknown");
    CHECK(doc["input"]["size"].getInt64() == static_cast<std::int64_t>(text.size()));
    CHECK_THAT(r.stderr_output, Catch::Matchers::ContainsSubstring("no known audio container"));

    const auto missing =
        run_pulp({"audio", "inspect", (path.parent_path() / "does-not-exist.wav").string()});
    CHECK(missing.exit_code == 1);
    CHECK_THAT(missing.stderr_output, Catch::Matchers::ContainsSubstring("not a readable file"));

    const auto none = run_pulp({"audio", "inspect"});
    CHECK(none.exit_code == 1);
    fs::remove(path);
}

TEST_CASE("audio inspect prints a compact table by default", "[cli][shellout][audio-inspect]") {
    if (!binary_exists())
        SKIP("pulp not built");
    const auto path = write_bytes("table.wav", valid_wav());
    const auto r = run_pulp({"audio", "inspect", path.string()});
    REQUIRE(r.exit_code == 0);
    CHECK_THAT(r.stdout_output, Catch::Matchers::ContainsSubstring("profile pcm44"));
    CHECK_THAT(r.stdout_output, Catch::Matchers::ContainsSubstring("decodable: yes"));
    CHECK_THAT(r.stdout_output, Catch::Matchers::ContainsSubstring("regions:"));
    CHECK_THAT(r.stdout_output, Catch::Matchers::ContainsSubstring("fmt "));
    CHECK_THAT(r.stdout_output, !Catch::Matchers::ContainsSubstring("warnings:"));
    fs::remove(path);
}
