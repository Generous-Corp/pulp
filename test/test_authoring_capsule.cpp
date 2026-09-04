/// @file test_authoring_capsule.cpp
/// Correctness suite for `pulp::authoring_capsule`.
///
/// The substrate's whole value is that a capsule from an untrusted machine is
/// admitted only when every one of its claims checks out, and that two exports
/// of an unchanged project agree on one identity. Both properties fail
/// silently when they break — a path that should have been refused extracts
/// fine, and a digest that drifted still looks like a digest — so each rule is
/// pinned here against an input that exercises exactly it.
///
/// Two conventions run through the file:
///
///   * Every rejection is asserted with its exact `CapsuleStatus`. A blanket
///     refusal would satisfy a test that only checked "this failed", so the
///     path cases additionally pin the rule token the rejection names: that is
///     what proves the admission gate distinguished the hostile class rather
///     than refusing everything for one reason.
///   * Every negative case is paired with a control that must succeed on the
///     same instrument. A collision check that rejected every set, or a decoder
///     that refused every file, would otherwise read as a passing suite.

#include <pulp/authoring_capsule/archive.hpp>
#include <pulp/authoring_capsule/canonical_pcm.hpp>
#include <pulp/authoring_capsule/capsule.hpp>
#include <pulp/authoring_capsule/component.hpp>
#include <pulp/authoring_capsule/limits.hpp>
#include <pulp/authoring_capsule/manifest.hpp>
#include <pulp/authoring_capsule/preview.hpp>
#include <pulp/authoring_capsule/profile_registry.hpp>
#include <pulp/authoring_capsule/safe_path.hpp>
#include <pulp/authoring_capsule/status.hpp>

// The canonical-JSON unit is module-private but deliberately exposes its
// number and serialization rules "so a test can pin the rule directly rather
// than inferring it from a whole envelope". Reaching it needs the module's own
// source directory on the include path; the test manifest adds it.
#include "canonical_json.hpp"

#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::authoring_capsule;
namespace fs = std::filesystem;

namespace {

// ── Rule tokens ─────────────────────────────────────────────────────────────
//
// `admit_member_path` reports the rule that refused a path in
// `CapsuleError::required`. The tokens are the admission gate's stable
// machine-readable vocabulary, and they are the only thing that tells one
// hostile class from another: without them a gate that rejected every path for
// one blanket reason would pass every case below.

constexpr std::string_view kRuleNonEmpty = "non-empty-path";
constexpr std::string_view kRuleMaxBytes = "max-path-bytes";
constexpr std::string_view kRuleMaxDepth = "max-path-depth";
constexpr std::string_view kRuleNfcSubset = "nfc-verifiable-subset";
constexpr std::string_view kRuleControl = "no-control-characters";
constexpr std::string_view kRuleBackslash = "forward-slash-separator";
constexpr std::string_view kRuleUnc = "no-unc-prefix";
constexpr std::string_view kRuleDriveLetter = "no-drive-letter";
constexpr std::string_view kRuleRelative = "relative-path";
constexpr std::string_view kRuleTrailingSep = "no-trailing-separator";
constexpr std::string_view kRuleEmptyComponent = "non-empty-component";
constexpr std::string_view kRuleDotComponent = "no-dot-component";
constexpr std::string_view kRuleTrailingDotSp = "no-trailing-dot-or-space";

/// Assert one hostile path is refused, and that the refusal names the class
/// rather than a generic failure.
void expect_path_rejected(std::string_view raw, std::string_view rule) {
    const auto admitted = admit_member_path(raw);
    REQUIRE_FALSE(admitted.has_value());
    CHECK(admitted.error().status == CapsuleStatus::path_rejected);
    CHECK(admitted.error().required == rule);
}

void expect_path_admitted(std::string_view raw) {
    const auto admitted = admit_member_path(raw);
    REQUIRE(admitted.has_value());
    // Verified, never rewritten: the manifest digest covers the path the
    // exporter wrote, so a normalizing admission gate would silently change
    // the identity of a capsule it accepted.
    CHECK(admitted.value() == std::string(raw));
}

// ── Filesystem scratch ──────────────────────────────────────────────────────

/// A unique directory that disappears with the test, so a capsule written by
/// one case can never be read by another.
class TempDir {
public:
    TempDir() {
        std::random_device entropy;
        root_ = fs::temp_directory_path() /
                ("pulp-authoring-capsule-" + std::to_string(entropy()) + "-" +
                 std::to_string(entropy()));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    const fs::path& path() const noexcept { return root_; }

private:
    fs::path root_;
};

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// Every path under `root`, relative and sorted: a whole-tree fingerprint, so a
// stray write anywhere is visible rather than only a write we thought to name.
std::vector<std::string> tree_snapshot(const fs::path& root) {
    std::vector<std::string> entries;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        entries.push_back(fs::relative(entry.path(), root).generic_string());
    std::sort(entries.begin(), entries.end());
    return entries;
}

// Runs a scope with the process working directory moved, so a scratch file
// written to a relative path lands where a snapshot can see it.
class ScopedWorkingDirectory {
public:
    explicit ScopedWorkingDirectory(const fs::path& next) : previous_(fs::current_path()) {
        fs::current_path(next);
    }

    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

    ~ScopedWorkingDirectory() {
        std::error_code ec;
        fs::current_path(previous_, ec);
    }

private:
    fs::path previous_;
};

// ── Manifest fixtures ───────────────────────────────────────────────────────

ComponentPolicy included_policy(Redistribution redistribution,
                                std::vector<RequiredFor> required_for) {
    ComponentPolicy policy;
    policy.canonicality = Canonicality::canonical_input;
    policy.source_availability = SourceAvailability::included;
    policy.editability = Editability::editable;
    policy.disclosure = Disclosure::public_;
    policy.redistribution = redistribution;
    policy.license_expression = "MIT";
    policy.required_for = std::move(required_for);
    return policy;
}

FileEntry file_row(std::string path, std::string role = "dsp.source") {
    FileEntry entry;
    entry.role = std::move(role);
    entry.path = std::move(path);
    entry.sha256 = std::string(64, 'a');
    entry.bytes = 4;
    entry.media_type = "application/octet-stream";
    entry.policy = included_policy(Redistribution::granted(), {RequiredFor::rebuild});
    return entry;
}

DependencyEntry dependency_row(std::string id) {
    DependencyEntry entry;
    entry.role = "sample.bank";
    entry.id = std::move(id);
    entry.sha256 = std::string(64, 'b');
    entry.bytes = 8;
    entry.media_type = "audio/x-pulp-canonical-pcm";
    entry.provider = "https://example.invalid/library";
    entry.required = true;
    entry.policy = included_policy(Redistribution::granted(), {RequiredFor::play});
    entry.policy.source_availability = SourceAvailability::external;
    return entry;
}

/// A structurally valid manifest with every digest-covered field populated, so
/// a test that changes exactly one field is changing something the digest is
/// supposed to see.
Manifest base_manifest() {
    Manifest manifest;
    manifest.profile = "com.example.capsule.test";
    manifest.profile_version = 1;
    manifest.product = "test-product";
    manifest.authoring_kind = "instrument";
    manifest.subtypes = {"sampler"};
    manifest.required_capabilities = {"pcm-sample-bank"};
    manifest.project_id = "project-42";
    manifest.reproducibility = Reproducibility::best_effort;
    manifest.compatibility.min_product_version = "1.0.0";
    manifest.compatibility.min_runtime_version = "1.0.0";
    manifest.compatibility.schema_version = "1";
    manifest.title = "A Capsule";
    manifest.created_at = "2026-01-01T00:00:00Z";
    manifest.exported_at = "2026-01-02T03:04:05Z";
    manifest.provenance_json = R"({"tool":"pulp"})";
    manifest.distribution_json = R"({"policy":"share-for-remix"})";
    manifest.unknown_optional_json = R"({"vendor_note":"one"})";
    manifest.files = {file_row("dsp/main.cpp"), file_row("ui/panel.js", "ui.source")};
    manifest.dependencies = {dependency_row("sha256-aaa"), dependency_row("sha256-bbb")};
    return manifest;
}

std::string digest_or_fail(const Manifest& manifest) {
    auto digest = revision_digest(manifest);
    REQUIRE(digest.has_value());
    return digest.value();
}

std::string canonical_or_fail(const Manifest& manifest) {
    auto canonical = to_canonical_json(manifest);
    REQUIRE(canonical.has_value());
    return canonical.value();
}

bool is_bare_lowercase_sha256(std::string_view text) {
    if (text.size() != 64) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// ── Profile registration ────────────────────────────────────────────────────

/// The smallest validator that answers every question `preview_capsule` asks.
/// It executes nothing, which is the contract: the registry supplies meaning,
/// never behaviour.
class TestProfileValidator final : public ProfileValidator {
public:
    static constexpr std::string_view kId = "com.example.capsule.test";

    std::string_view profile_id() const noexcept override { return kId; }
    std::uint32_t max_profile_version() const noexcept override { return 1; }
    std::vector<std::string> required_roles() const override { return {"dsp.source"}; }
    bool supports_capability(std::string_view name) const noexcept override {
        return name == "pcm-sample-bank";
    }
    pulp::runtime::Result<void, CapsuleError> check_compatibility(const Manifest&) const override {
        return {};
    }
    pulp::runtime::Result<void, CapsuleError> validate_staged(const Manifest&,
                                                              const fs::path&) const override {
        return {};
    }
};

ProfileRegistry test_registry() {
    ProfileRegistry registry;
    registry.register_profile(std::make_shared<TestProfileValidator>());
    return registry;
}

// ── WAV construction ────────────────────────────────────────────────────────

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

void put_id(std::vector<std::uint8_t>& out, std::string_view id) {
    out.insert(out.end(), id.begin(), id.end());
}

/// A minimal canonical RIFF/WAVE container. The decoder under test owns its own
/// reader, so the fixture is written by hand rather than through any Pulp
/// encoder: a shared writer would let one bug cancel the other out.
std::vector<std::uint8_t> make_wav(std::uint16_t format_tag, std::uint16_t bits_per_sample,
                                   std::uint16_t channels, std::uint32_t sample_rate,
                                   const std::vector<std::uint8_t>& data) {
    const auto block_align = static_cast<std::uint16_t>(channels * (bits_per_sample / 8));
    std::vector<std::uint8_t> out;
    put_id(out, "RIFF");
    put_u32(out, static_cast<std::uint32_t>(4 + 24 + 8 + data.size()));
    put_id(out, "WAVE");
    put_id(out, "fmt ");
    put_u32(out, 16);
    put_u16(out, format_tag);
    put_u16(out, channels);
    put_u32(out, sample_rate);
    put_u32(out, sample_rate * block_align);
    put_u16(out, block_align);
    put_u16(out, bits_per_sample);
    put_id(out, "data");
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::uint8_t> pcm16_data(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> out;
    for (const auto sample : samples)
        put_u16(out, static_cast<std::uint16_t>(sample));
    return out;
}

std::vector<std::uint8_t> pcm24_data(const std::vector<std::int32_t>& samples) {
    std::vector<std::uint8_t> out;
    for (const auto sample : samples) {
        const auto raw = static_cast<std::uint32_t>(sample);
        out.push_back(static_cast<std::uint8_t>(raw & 0xFF));
        out.push_back(static_cast<std::uint8_t>((raw >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((raw >> 16) & 0xFF));
    }
    return out;
}

std::vector<std::uint8_t> pcm32_data(const std::vector<std::int32_t>& samples) {
    std::vector<std::uint8_t> out;
    for (const auto sample : samples)
        put_u32(out, static_cast<std::uint32_t>(sample));
    return out;
}

std::vector<std::uint8_t> float32_data(const std::vector<float>& samples) {
    std::vector<std::uint8_t> out;
    for (const auto sample : samples)
        put_u32(out, std::bit_cast<std::uint32_t>(sample));
    return out;
}

CanonicalPcm make_pcm(std::uint32_t channels, std::uint32_t rate, std::vector<float> samples) {
    CanonicalPcm pcm;
    pcm.channels = channels;
    pcm.sample_rate = rate;
    pcm.frame_count = samples.size() / channels;
    pcm.samples = std::move(samples);
    return pcm;
}

/// Moderately compressible payload: deflate must beat store (so the writer
/// picks deflate when it has room) while the expansion ratio stays far below
/// the 200:1 admission ceiling (so the reader still opens the archive). A
/// four-symbol alphabet lands between those two walls on purpose.
std::vector<std::uint8_t> compressible_payload(std::size_t size) {
    std::vector<std::uint8_t> out;
    out.reserve(size);
    std::uint32_t state = 0x1234567u;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        out.push_back(static_cast<std::uint8_t>('a' + ((state >> 16) & 0x3u)));
    }
    return out;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Path admission
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("admit_member_path accepts generated capsule member paths",
          "[authoring-capsule][safe-path]") {
    // The control for every rejection below. If these failed, the rejection
    // cases would pass for the wrong reason.
    expect_path_admitted("capsule.json");
    expect_path_admitted("audio/samples/0001.pcm");
    expect_path_admitted("dsp/main.cpp");
    // Latin-1 and Latin Extended-A precomposed letters are provably NFC.
    expect_path_admitted("naïve/über.txt");
    // Hiragana, katakana with the prolonged sound mark, and CJK unified.
    expect_path_admitted("音/データー.bin");
    // Exactly at the depth budget rather than one past it.
    {
        std::string deep = "a";
        for (std::size_t i = 1; i < kCapsuleLimitsV1.max_path_depth; ++i) deep += "/a";
        expect_path_admitted(deep);
    }
}

TEST_CASE("admit_member_path rejects each hostile path class by name",
          "[authoring-capsule][safe-path]") {
    SECTION("traversal") {
        expect_path_rejected("../secret.key", kRuleDotComponent);
        expect_path_rejected("audio/../../secret.key", kRuleDotComponent);
        expect_path_rejected("./payload.bin", kRuleDotComponent);
    }
    SECTION("absolute") {
        expect_path_rejected("/etc/passwd", kRuleRelative);
    }
    SECTION("drive letter") {
        expect_path_rejected("C:/Windows/system32/x.dll", kRuleDriveLetter);
        expect_path_rejected("c:payload.bin", kRuleDriveLetter);
    }
    SECTION("UNC prefix") {
        expect_path_rejected("//server/share/payload.bin", kRuleUnc);
        expect_path_rejected("\\\\server\\share\\payload.bin", kRuleUnc);
    }
    SECTION("backslash separator") {
        expect_path_rejected("audio\\payload.bin", kRuleBackslash);
    }
    SECTION("NUL") {
        expect_path_rejected(std::string_view("audio/pay\0load.bin", 18), kRuleControl);
    }
    SECTION("C0 control") {
        expect_path_rejected("audio/pay\x01load.bin", kRuleControl);
        expect_path_rejected("audio/pay\x1bload.bin", kRuleControl);
    }
    SECTION("empty component") {
        expect_path_rejected("audio//payload.bin", kRuleEmptyComponent);
        expect_path_rejected("", kRuleNonEmpty);
        // A trailing separator is its own class: the component after it is
        // empty, but the shape is a directory reference, not a typo.
        expect_path_rejected("audio/", kRuleTrailingSep);
    }
    SECTION("trailing dot or space") {
        // Windows strips both, so `evil.` and `evil` would land on one file.
        expect_path_rejected("audio/payload.", kRuleTrailingDotSp);
        expect_path_rejected("audio/payload ", kRuleTrailingDotSp);
        expect_path_rejected("audio /payload.bin", kRuleTrailingDotSp);
    }
    SECTION("over depth") {
        std::string deep = "a";
        for (std::size_t i = 0; i < kCapsuleLimitsV1.max_path_depth; ++i) deep += "/a";
        expect_path_rejected(deep, kRuleMaxDepth);
    }
    SECTION("over length") {
        expect_path_rejected(std::string(kCapsuleLimitsV1.max_path_bytes + 1, 'a'), kRuleMaxBytes);
    }
    SECTION("not provably NFC") {
        // `e` + U+0301 is the decomposed spelling of `é`. No character database
        // is linked here, so it cannot be normalized and is refused instead of
        // being half-repaired.
        expect_path_rejected("audio/cafe\xCC\x81.bin", kRuleNfcSubset);
        // Cyrillic is outside the vetted allowlist even though every character
        // in it is already NFC: the subset is what can be proven, not what is.
        expect_path_rejected("аudio/payload.bin", kRuleNfcSubset);
    }
}

TEST_CASE("collision detection folds case and confusables", "[authoring-capsule][safe-path]") {
    SECTION("distinct members are admitted") {
        // The control: a folding rule that collapsed everything would reject
        // this set too, and both cases below would pass for the wrong reason.
        const std::vector<std::string> distinct{"audio/one.pcm", "audio/two.pcm", "dsp/main.cpp"};
        CHECK(check_collisions(distinct).has_value());
    }

    SECTION("ASCII case pair") {
        const std::vector<std::string> pair{"audio/Payload.bin", "audio/payload.bin"};
        CHECK(collision_key(pair[0]) == collision_key(pair[1]));
        const auto checked = check_collisions(pair);
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().status == CapsuleStatus::path_collision);
        CHECK(checked.error().subject == pair[0]);
        CHECK(checked.error().found == pair[1]);
    }

    SECTION("Unicode confusable pair") {
        // U+017F LATIN SMALL LETTER LONG S reads as `s` in any file listing.
        const std::vector<std::string> pair{"audio/me\xC5\xBFh.bin", "audio/mesh.bin"};
        CHECK(collision_key(pair[0]) == collision_key(pair[1]));
        const auto checked = check_collisions(pair);
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().status == CapsuleStatus::path_collision);
    }

    SECTION("digit and letter confusable pair") {
        const std::vector<std::string> pair{"audio/l0go.bin", "audio/1ogo.bin"};
        CHECK(collision_key(pair[0]) == collision_key(pair[1]));
        CHECK_FALSE(check_collisions(pair).has_value());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Canonical JSON
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("canonical JSON sorts keys by code point and emits no whitespace",
          "[authoring-capsule][canonical-json]") {
    auto object = choc::value::createObject("");
    object.addMember("b", 1);
    object.addMember("a", 2);
    object.addMember("Z", 3);
    object.addMember("A", 4);
    object.addMember("é", 5);

    auto text = detail::to_canonical_text(object.getView());
    REQUIRE(text.has_value());
    // Uppercase sorts before lowercase and the multi-byte key sorts last,
    // because the order is over raw UTF-8 octets — which is code point order.
    CHECK(text.value() == R"({"A":4,"Z":3,"a":2,"b":1,"é":5})");

    // Insignificant whitespace is removed rather than preserved: the digest is
    // taken over these bytes, so two writers that space their output
    // differently must still agree on one identity.
    auto normalized = detail::canonicalize_json_text("  {\n  \"b\" : 1 ,\n  \"a\" : [ 1 , 2 ]\n} ");
    REQUIRE(normalized.has_value());
    CHECK(normalized.value() == R"({"a":[1,2],"b":1})");
}

TEST_CASE("canonical JSON numbers are the shortest round-tripping decimal",
          "[authoring-capsule][canonical-json]") {
    const auto canonical = [](double value) {
        auto text = detail::canonical_number(value);
        REQUIRE(text.has_value());
        return text.value();
    };

    // An exact integer never grows a decimal point or an exponent.
    CHECK(canonical(1.0) == "1");
    CHECK(canonical(100.0) == "100");
    CHECK(canonical(-0.0) == "0");
    CHECK(canonical(1.5) == "1.5");
    // No digit the value does not require.
    CHECK(canonical(0.1) == "0.1");
    CHECK(canonical(0.3) == "0.3");
    // The fixed/exponential boundary is the ECMAScript rule, not the
    // platform's printf default.
    CHECK(canonical(1e20) == "100000000000000000000");
    CHECK(canonical(1e21) == "1e+21");
    CHECK(canonical(1e-6) == "0.000001");
    CHECK(canonical(1e-7) == "1e-7");
}

TEST_CASE("canonical JSON rejects NaN and infinity", "[authoring-capsule][canonical-json]") {
    const auto nan = detail::canonical_number(std::numeric_limits<double>::quiet_NaN());
    REQUIRE_FALSE(nan.has_value());
    CHECK(nan.error().status == CapsuleStatus::manifest_invalid);
    CHECK(nan.error().found == "NaN");

    const auto infinity = detail::canonical_number(std::numeric_limits<double>::infinity());
    REQUIRE_FALSE(infinity.has_value());
    CHECK(infinity.error().status == CapsuleStatus::manifest_invalid);
    CHECK(infinity.error().found == "infinity");

    const auto negative = detail::canonical_number(-std::numeric_limits<double>::infinity());
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().status == CapsuleStatus::manifest_invalid);

    // A non-finite number nested in a document is refused with the pointer of
    // the member that carries it, not swallowed as a zero.
    auto object = choc::value::createObject("");
    object.addMember("gain", std::numeric_limits<double>::infinity());
    const auto serialized = detail::to_canonical_text(object.getView());
    REQUIRE_FALSE(serialized.has_value());
    CHECK(serialized.error().status == CapsuleStatus::manifest_invalid);
    CHECK(serialized.error().subject == "/gain");

    // Control: the same document with a finite number serializes.
    auto finite = choc::value::createObject("");
    finite.addMember("gain", 0.5);
    const auto ok = detail::to_canonical_text(finite.getView());
    REQUIRE(ok.has_value());
    CHECK(ok.value() == R"({"gain":0.5})");
}

// ═══════════════════════════════════════════════════════════════════════════
// Manifest round-trip
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// A hand-written, deliberately non-canonical envelope: keys out of order,
/// pretty-printed, carrying two unknown optional keys a newer writer might
/// emit, a declared `completeness`, and no `title`.
constexpr std::string_view kRoundTripJson = R"({
  "product": "test-product",
  "format_version": 1,
  "format": "org.pulp.audio-authoring-capsule",
  "profile": "com.example.capsule.test",
  "profile_version": 1,
  "authoring_kind": "instrument",
  "project_id": "project-42",
  "completeness": "self_contained",
  "vendor_note": { "b": 2, "a": 1 },
  "future_flag": true,
  "created_at": "2026-01-01T00:00:00Z",
  "exported_at": "2026-01-02T03:04:05Z",
  "files": [
    {
      "role": "dsp.source",
      "path": "dsp/main.cpp",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "bytes": 12,
      "media_type": "text/x-c++src",
      "policy": {
        "source_availability": "included",
        "redistribution": "allowed",
        "required_for": ["rebuild"]
      }
    }
  ],
  "dependencies": []
})";

}  // namespace

TEST_CASE("manifest round-trip is canonical-identical and preserves unknown keys",
          "[authoring-capsule][manifest]") {
    auto first = parse_manifest(kRoundTripJson);
    REQUIRE(first.has_value());

    const std::string once = canonical_or_fail(first.value());
    auto second = parse_manifest(once);
    REQUIRE(second.has_value());
    const std::string twice = canonical_or_fail(second.value());

    // Canonical form is a fixed point: a second pass changes nothing.
    CHECK(once == twice);

    // It is NOT byte-identical to the authored input, and must not be — the
    // canonical form is total, so it fills in defaults the author omitted.
    CHECK(once != std::string(kRoundTripJson));

    // An unknown optional key survives verbatim, canonicalized in place, so a
    // re-export through an older reader does not drop what a newer writer
    // emitted.
    CHECK(once.find(R"("vendor_note":{"a":1,"b":2})") != std::string::npos);
    CHECK(once.find(R"("future_flag":true)") != std::string::npos);
    CHECK(second.value().unknown_optional_json.find("vendor_note") != std::string::npos);

    // An omitted optional key comes back with its default rather than staying
    // absent: totality is what lets the digest be defined over the whole
    // envelope.
    CHECK(first.value().title.empty());
    CHECK(once.find(R"("title":"")") != std::string::npos);

    // A declared completeness is the one deliberate exception. It is derived
    // from the rows, so keeping it would let a stale claim outlive them.
    CHECK(once.find("completeness") == std::string::npos);
}

TEST_CASE("a files row that is not included is manifest_invalid",
          "[authoring-capsule][manifest]") {
    // Only a dependencies[] row carries a provider, so a files[] row that
    // declares its bytes are elsewhere names a component nothing could ever
    // resolve. Admitting it would publish an unfixable capsule.
    for (const std::string_view availability : {"external", "local-only", "omitted"}) {
        std::string json(kRoundTripJson);
        const auto at = json.find(R"("source_availability": "included")");
        REQUIRE(at != std::string::npos);
        json.replace(at, std::strlen(R"("source_availability": "included")"),
                     std::string(R"("source_availability": ")") + std::string(availability) + "\"");

        const auto parsed = parse_manifest(json);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().status == CapsuleStatus::manifest_invalid);
        CHECK(parsed.error().subject == "/files/0/policy/source_availability");
        CHECK(parsed.error().required == "included");
        CHECK(parsed.error().found == std::string(availability));
    }

    // Control: the unmodified envelope parses, so the rejections above are
    // about the availability field and nothing else.
    CHECK(parse_manifest(kRoundTripJson).has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Revision identity
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("digest spelling is prefixed for identity references and bare for content",
          "[authoring-capsule][digest]") {
    Manifest manifest = base_manifest();
    manifest.parent_revision = "sha256:" + std::string(64, 'c');
    const std::string digest = digest_or_fail(manifest);

    // `revision_id` and `parent_revision` are identity references that may one
    // day name another algorithm, so they carry the algorithm with them.
    REQUIRE(digest.rfind("sha256:", 0) == 0);
    CHECK(is_bare_lowercase_sha256(digest.substr(std::strlen("sha256:"))));

    manifest.revision_id = digest;
    const std::string canonical = canonical_or_fail(manifest);
    CHECK(canonical.find(R"("revision_id":"sha256:)") != std::string::npos);
    CHECK(canonical.find(R"("parent_revision":"sha256:)") != std::string::npos);

    // Every `sha256` field is bare lowercase hex: the field name has already
    // said the algorithm.
    for (const auto& file : manifest.files) CHECK(is_bare_lowercase_sha256(file.sha256));
    CHECK(canonical.find(R"("sha256":"sha256:)") == std::string::npos);
}

TEST_CASE("dependencies are sorted by id, so authored order cannot change identity",
          "[authoring-capsule][digest]") {
    Manifest ascending = base_manifest();
    Manifest descending = ascending;
    std::reverse(descending.dependencies.begin(), descending.dependencies.end());

    REQUIRE(ascending.dependencies.front().id != descending.dependencies.front().id);
    CHECK(digest_or_fail(ascending) == digest_or_fail(descending));
    CHECK(canonical_or_fail(ascending) == canonical_or_fail(descending));
}

TEST_CASE("revision_digest is stable across the fields it excludes",
          "[authoring-capsule][digest]") {
    const Manifest base = base_manifest();
    const std::string reference = digest_or_fail(base);

    SECTION("exported_at") {
        Manifest later = base;
        later.exported_at = "2027-12-31T23:59:59Z";
        CHECK(digest_or_fail(later) == reference);
    }
    SECTION("revision_id the capsule asserts about itself") {
        Manifest asserted = base;
        asserted.revision_id = "sha256:" + std::string(64, 'd');
        CHECK(digest_or_fail(asserted) == reference);
    }
    SECTION("attestations") {
        Manifest signed_capsule = base;
        signed_capsule.attestations_json =
            R"([{"algorithm":"ed25519","signature":"AA==","signer_id":"someone"}])";
        CHECK(digest_or_fail(signed_capsule) == reference);
    }
    SECTION("files[] input ordering") {
        Manifest reordered = base;
        std::reverse(reordered.files.begin(), reordered.files.end());
        CHECK(digest_or_fail(reordered) == reference);
    }
}

TEST_CASE("revision_digest covers every other field", "[authoring-capsule][digest]") {
    const Manifest base = base_manifest();
    const std::string reference = digest_or_fail(base);

    SECTION("title") {
        Manifest changed = base;
        changed.title = "A Different Capsule";
        CHECK(digest_or_fail(changed) != reference);
    }
    SECTION("created_at") {
        Manifest changed = base;
        changed.created_at = "2020-06-06T00:00:00Z";
        CHECK(digest_or_fail(changed) != reference);
    }
    SECTION("provenance") {
        // Provenance carries disclosure state; leaving it uncovered would let
        // it be rewritten under a valid signature.
        Manifest changed = base;
        changed.provenance_json = R"({"tool":"something-else"})";
        CHECK(digest_or_fail(changed) != reference);
    }
    SECTION("distribution") {
        // `distribution` is what makes a capsule Play-only. Outside the digest,
        // a Play-only release could be edited into a remixable one and still
        // verify.
        Manifest changed = base;
        changed.distribution_json = R"({"policy":"private-backup"})";
        CHECK(digest_or_fail(changed) != reference);
    }
    SECTION("a file row's executable_data") {
        // The flag is what tells a person the payload contains code. Outside
        // the digest, it could be dropped from a signed capsule to hide that.
        Manifest changed = base;
        changed.files.front().executable_data = !changed.files.front().executable_data;
        CHECK(digest_or_fail(changed) != reference);
    }
    SECTION("an unknown optional key") {
        // A key this build does not understand is still content someone signed.
        Manifest changed = base;
        changed.unknown_optional_json = R"({"vendor_note":"two"})";
        CHECK(digest_or_fail(changed) != reference);

        Manifest added = base;
        added.unknown_optional_json = R"({"vendor_note":"one","vendor_extra":1})";
        CHECK(digest_or_fail(added) != reference);
    }
    SECTION("routing and closure fields") {
        Manifest retitled_role = base;
        retitled_role.files.front().role = "dsp.header";
        CHECK(digest_or_fail(retitled_role) != reference);

        Manifest reproducibility = base;
        reproducibility.reproducibility = Reproducibility::frozen_output_only;
        CHECK(digest_or_fail(reproducibility) != reference);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Completeness
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("derive_completeness reaches each verdict from the component rows",
          "[authoring-capsule][completeness]") {
    SECTION("self_contained") {
        Manifest manifest;
        FileEntry entry = file_row("dsp/main.cpp");
        entry.policy =
            included_policy(Redistribution::granted(), {RequiredFor::play, RequiredFor::rebuild});
        manifest.files = {entry};
        CHECK(derive_completeness(manifest) == Completeness::self_contained);
    }

    SECTION("redistribution unknown never yields self_contained") {
        // `unknown` is a real state: an absent statement of rights is not a
        // grant, and no projection later can recover a permission this layer
        // refused to invent.
        Manifest manifest;
        FileEntry entry = file_row("dsp/main.cpp");
        entry.policy =
            included_policy(Redistribution::unknown(), {RequiredFor::play, RequiredFor::rebuild});
        manifest.files = {entry};
        const auto verdict = derive_completeness(manifest);
        CHECK(verdict != Completeness::self_contained);
        CHECK(verdict == Completeness::partial);

        // Same rows, `restricted` instead of `unknown`: also not self-contained.
        manifest.files.front().policy.redistribution = Redistribution::restricted();
        CHECK(derive_completeness(manifest) != Completeness::self_contained);

        // Control: flipping only the redistribution grant reaches
        // self_contained, so the two checks above are about rights and not
        // about some unrelated defect in the rows.
        manifest.files.front().policy.redistribution = Redistribution::granted();
        CHECK(derive_completeness(manifest) == Completeness::self_contained);
    }

    SECTION("resolvable") {
        Manifest manifest;
        DependencyEntry entry = dependency_row("sha256-aaa");
        entry.policy =
            included_policy(Redistribution::granted(), {RequiredFor::play, RequiredFor::rebuild});
        entry.policy.source_availability = SourceAvailability::external;
        manifest.dependencies = {entry};
        CHECK(derive_completeness(manifest) == Completeness::resolvable);

        // A stable identity with no resolver is not resolvable.
        manifest.dependencies.front().provider.clear();
        CHECK(derive_completeness(manifest) == Completeness::partial);
    }

    SECTION("play_only") {
        Manifest manifest;
        FileEntry rendition = file_row("audio/render.pcm", "audio.rendition");
        rendition.policy = included_policy(Redistribution::unknown(), {RequiredFor::play});
        rendition.policy.canonicality = Canonicality::derived_output;

        DependencyEntry source = dependency_row("sha256-source");
        source.policy = included_policy(Redistribution::unknown(), {RequiredFor::rebuild});
        source.policy.canonicality = Canonicality::canonical_input;
        source.policy.source_availability = SourceAvailability::omitted;
        source.provider.clear();

        manifest.files = {rendition};
        manifest.dependencies = {source};
        CHECK(derive_completeness(manifest) == Completeness::play_only);
    }

    SECTION("partial") {
        Manifest manifest;
        FileEntry entry = file_row("audio/render.pcm", "audio.rendition");
        entry.policy = included_policy(Redistribution::granted(), {RequiredFor::play});
        // The bytes never left the exporting machine, and nothing names a way
        // to fetch them: the capsule cannot even play.
        entry.policy.source_availability = SourceAvailability::local_only;
        manifest.files = {entry};
        CHECK(derive_completeness(manifest) == Completeness::partial);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Canonical PCM
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("canonical PCM decodes each admissible WAV source format",
          "[authoring-capsule][canonical-pcm]") {
    SECTION("PCM16") {
        const auto wav = make_wav(0x0001, 16, 1, 48000, pcm16_data({0, 16384, -32768, -16384}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels == 1);
        CHECK(decoded->sample_rate == 48000);
        CHECK(decoded->frame_count == 4);
        // The scale is 2^-15, so every one of these is exact.
        CHECK(decoded->samples == std::vector<float>{0.0f, 0.5f, -1.0f, -0.5f});
    }

    SECTION("PCM24") {
        const auto wav = make_wav(0x0001, 24, 1, 44100, pcm24_data({0, 4194304, -8388608}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->frame_count == 3);
        CHECK(decoded->samples == std::vector<float>{0.0f, 0.5f, -1.0f});
    }

    SECTION("PCM32") {
        const auto wav = make_wav(0x0001, 32, 1, 96000,
                                  pcm32_data({0, 1073741824, std::numeric_limits<int>::min()}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->sample_rate == 96000);
        CHECK(decoded->samples == std::vector<float>{0.0f, 0.5f, -1.0f});
    }

    SECTION("float32, stereo") {
        const auto wav =
            make_wav(0x0003, 32, 2, 44100, float32_data({0.25f, -0.75f, 1.0f, -0.125f}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels == 2);
        CHECK(decoded->frame_count == 2);
        CHECK(decoded->samples == std::vector<float>{0.25f, -0.75f, 1.0f, -0.125f});
    }
}

TEST_CASE("canonical PCM bytes round-trip exactly", "[authoring-capsule][canonical-pcm]") {
    const CanonicalPcm original =
        make_pcm(2, 48000, {0.0f, -1.0f, 0.5f, 0.25f, -0.125f, 0.999969482421875f});
    REQUIRE(validate_canonical(original).has_value());

    const auto bytes = to_canonical_bytes(original);
    // Header first, then the samples, with nothing after them: the member is
    // self-describing, so its geometry travels inside the hashed bytes.
    CHECK(bytes.size() == kCanonicalPcmHeaderBytes + original.samples.size() * sizeof(float));
    REQUIRE(std::equal(std::begin(kCanonicalPcmMagic), std::end(kCanonicalPcmMagic),
                       bytes.begin()));

    const auto restored = from_canonical_bytes(bytes);
    REQUIRE(restored.has_value());
    CHECK(restored->channels == original.channels);
    CHECK(restored->sample_rate == original.sample_rate);
    CHECK(restored->frame_count == original.frame_count);
    // Bit-exact, not approximate: the digest is over these bytes.
    CHECK(restored->samples == original.samples);
    CHECK(canonical_pcm_digest(*restored) == canonical_pcm_digest(original));
}

TEST_CASE("canonical PCM refuses NaN and infinity", "[authoring-capsule][canonical-pcm]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();

    SECTION("through validate_canonical") {
        const auto with_nan = validate_canonical(make_pcm(1, 48000, {0.0f, nan}));
        REQUIRE_FALSE(with_nan.has_value());
        CHECK(with_nan.error().status == CapsuleStatus::decode_unsupported);
        CHECK(with_nan.error().found == "nan");

        const auto with_inf = validate_canonical(make_pcm(1, 48000, {infinity}));
        REQUIRE_FALSE(with_inf.has_value());
        CHECK(with_inf.error().status == CapsuleStatus::decode_unsupported);
        CHECK(with_inf.error().found == "inf");
    }

    SECTION("through a float32 WAV") {
        const auto wav = make_wav(0x0003, 32, 1, 48000, float32_data({0.5f, nan}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().status == CapsuleStatus::decode_unsupported);

        // Control: the same container with finite samples decodes, so the
        // refusal is about the NaN and not about the fixture.
        CHECK(decode_to_canonical(make_wav(0x0003, 32, 1, 48000, float32_data({0.5f, 0.25f})))
                  .has_value());
    }

    SECTION("through from_canonical_bytes") {
        // A well-formed header over a NaN payload: the geometry checks pass,
        // so the refusal can only come from the sample scan.
        CanonicalPcm poisoned = make_pcm(1, 48000, {nan});
        const auto restored = from_canonical_bytes(to_canonical_bytes(poisoned));
        REQUIRE_FALSE(restored.has_value());
        CHECK(restored.error().status == CapsuleStatus::decode_unsupported);

        // Control: the same header with a finite sample parses, so the
        // refusal is about the NaN and not about the fixture's header.
        CHECK(from_canonical_bytes(to_canonical_bytes(make_pcm(1, 48000, {0.5f}))).has_value());
    }
}

TEST_CASE("canonical PCM refuses an unsupported codec", "[authoring-capsule][canonical-pcm]") {
    // Mu-law, A-law, and IMA ADPCM are all real WAV payloads. Approximating
    // one would hand back plausible floats nobody asked for.
    for (const std::uint16_t tag : {std::uint16_t{0x0007}, std::uint16_t{0x0006},
                                    std::uint16_t{0x0011}, std::uint16_t{0x0055}}) {
        const auto wav = make_wav(tag, 8, 1, 44100, std::vector<std::uint8_t>(8, 0x80));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().status == CapsuleStatus::decode_unsupported);
        CHECK(decoded.error().subject == "wav.fmt.audio_format");
    }

    // A container this build does not admit is named too, rather than
    // silently producing zero frames. These three lose resolution or decode
    // differently across ISAs, so admitting them would let the same file mint
    // different identities on different machines.
    for (const auto& [source, name] :
         std::vector<std::pair<std::string_view, std::string>>{
             {"OggS and then some padding bytes", "ogg"},
             {"caff and then some padding bytes", "caf"},
             {"ID3 and then some padding bytes", "mp3:id3"}}) {
        const auto refused = decode_to_canonical(bytes_of(source));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::decode_unsupported);
        CHECK(refused.error().subject == "audio.container");
        CHECK(refused.error().found == name);
    }

    // A FLAC container is dispatched to the FLAC decoder — the refusal names
    // the stream rather than the container, which is how a caller tells a
    // corrupt admitted format from an unadmitted one.
    const auto corrupt_flac = decode_to_canonical(bytes_of("fLaC and then some padding bytes"));
    REQUIRE_FALSE(corrupt_flac.has_value());
    CHECK(corrupt_flac.error().status == CapsuleStatus::decode_unsupported);
    CHECK(corrupt_flac.error().subject == "flac.stream");
}

TEST_CASE("two renditions differing only in declared rate have different identities",
          "[authoring-capsule][canonical-pcm]") {
    const std::vector<float> samples{0.0f, 0.5f, -0.5f, 0.25f};
    const CanonicalPcm at_44100 = make_pcm(1, 44100, samples);
    const CanonicalPcm at_48000 = make_pcm(1, 48000, samples);

    // The sample payloads are byte-identical; only the header's declared rate
    // differs.
    const auto bytes_44100 = to_canonical_bytes(at_44100);
    const auto bytes_48000 = to_canonical_bytes(at_48000);
    REQUIRE(std::equal(bytes_44100.begin() + kCanonicalPcmHeaderBytes, bytes_44100.end(),
                       bytes_48000.begin() + kCanonicalPcmHeaderBytes, bytes_48000.end()));
    // The rate lives inside the hashed header precisely so identical samples
    // played at different rates cannot share an identity.
    CHECK(canonical_pcm_digest(at_44100) != canonical_pcm_digest(at_48000));

    // Channel count and frame count are covered for the same reason.
    CHECK(canonical_pcm_digest(make_pcm(2, 44100, samples)) != canonical_pcm_digest(at_44100));
}

// ═══════════════════════════════════════════════════════════════════════════
// Export, open, preview
// ═══════════════════════════════════════════════════════════════════════════

namespace {

ExportRequest make_export_request() {
    ExportRequest request;
    request.manifest = base_manifest();
    request.manifest.files.clear();
    request.manifest.dependencies = {dependency_row("sha256-aaa")};

    ExportItem dsp;
    dsp.entry = file_row("dsp/main.cpp");
    dsp.entry.media_type = "text/x-c++src";
    dsp.entry.executable_data = true;
    dsp.bytes = bytes_of("int main() { return 0; }\n");

    ExportItem audio;
    audio.entry = file_row("audio/render.pcm", "audio.rendition");
    audio.entry.media_type = "audio/x-pulp-canonical-pcm";
    audio.entry.policy = included_policy(Redistribution::granted(), {RequiredFor::play});
    audio.bytes = to_canonical_bytes(make_pcm(1, 48000, {0.0f, 0.5f, -0.5f}));

    // Deliberately not in path order: the exporter owns the canonical order.
    request.items = {audio, dsp};
    return request;
}

}  // namespace

TEST_CASE("export then open then preview admits a capsule end to end",
          "[authoring-capsule][preview]") {
    TempDir temp;
    const fs::path destination = temp.path() / "instrument.capsule";

    const auto written = export_capsule(make_export_request(), destination);
    REQUIRE(written.has_value());
    REQUIRE(fs::exists(destination));

    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());

    const auto members = archive->members();
    REQUIRE(members.size() == 3);
    // The manifest is member 0 so a reader can learn what a capsule is without
    // scanning a hostile central directory.
    CHECK(members[0].path == std::string(kManifestPath));
    CHECK(members[1].path == "audio/render.pcm");
    CHECK(members[2].path == "dsp/main.cpp");

    const ProfileRegistry registry = test_registry();
    AdmissionOptions options;
    options.product = "test-product";

    const auto preview = preview_capsule(*archive, registry, options);
    REQUIRE(preview.has_value());

    CHECK(preview->compatibility == CompatibilityVerdict::supported);
    CHECK(preview->unmet.status == CapsuleStatus::ok);
    CHECK(preview->member_count == 3);
    REQUIRE(preview->capabilities.size() == 1);
    CHECK(preview->capabilities.front().name == "pcm-sample-bank");
    CHECK(preview->capabilities.front().available);

    // Surfaced so consent is informed. It authorizes nothing.
    CHECK(preview->contains_executable_data);

    // No verifier is configured, so a capsule is admitted as unsigned rather
    // than as verified.
    CHECK_FALSE(preview->signature_verified);

    // The substrate opens no network connection and knows no resolver, so it
    // reports the honest answer and leaves the field to the consumer.
    REQUIRE(preview->dependencies.size() == 1);
    CHECK(preview->dependencies.front().id == "sha256-aaa");
    CHECK_FALSE(preview->dependencies.front().resolvable_locally);

    // The closure was measured from the bytes that travelled, not copied from
    // what the caller declared.
    REQUIRE(preview->manifest.files.size() == 2);
    for (const auto& entry : preview->manifest.files) {
        CHECK(is_bare_lowercase_sha256(entry.sha256));
        const auto expanded = archive->read(entry.path);
        REQUIRE(expanded.has_value());
        CHECK(entry.bytes == expanded->size());
        CHECK(entry.sha256 == pulp::runtime::sha256_hex(expanded->data(), expanded->size()));
    }

    // The identity the preview binds to is the one it recomputed.
    CHECK(preview->manifest.revision_id == digest_or_fail(preview->manifest));
}

TEST_CASE("a successful preview writes nothing while admission extracts the same member",
          "[authoring-capsule][preview]") {
    TempDir temp;
    const fs::path destination = temp.path() / "inert.capsule";
    REQUIRE(export_capsule(make_export_request(), destination).has_value());

    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    const fs::path extracted = staging->root() / "dsp" / "main.cpp";

    const ProfileRegistry registry = test_registry();
    AdmissionOptions options;
    options.product = "test-product";

    const ScopedWorkingDirectory working_directory(temp.path());
    const auto before = tree_snapshot(temp.path());

    const auto preview = preview_capsule(*archive, registry, options);
    REQUIRE(preview.has_value());
    CHECK(preview->compatibility == CompatibilityVerdict::supported);

    // The preview read and judged a member the manifest declares executable,
    // and left the tree exactly as it found it. Consent precedes extraction.
    CHECK(tree_snapshot(temp.path()) == before);
    CHECK(fs::is_empty(staging->root()));
    CHECK_FALSE(fs::exists(extracted));

    // The control, on the same archive and the same staging root: admission
    // does write, and writes that member. Without it, a preview that touched
    // nothing because the capsule carried nothing would read identically.
    REQUIRE(admit_to_staging(*archive, *preview, registry, *staging).has_value());
    CHECK(tree_snapshot(temp.path()) != before);
    REQUIRE(fs::is_regular_file(extracted));
    CHECK(read_file(extracted) == bytes_of("int main() { return 0; }\n"));
}

TEST_CASE("preview rejects a files row whose declared size disagrees with the member",
          "[authoring-capsule][preview]") {
    TempDir temp;
    const fs::path destination = temp.path() / "mismatched.capsule";

    const auto payload = bytes_of("twelve bytes");
    Manifest manifest = base_manifest();
    manifest.dependencies.clear();
    manifest.files.clear();

    FileEntry entry = file_row("payload.bin", "data.blob");
    entry.sha256 = pulp::runtime::sha256_hex(payload.data(), payload.size());
    // The lie: one byte more than the container will ever expand to. Nothing
    // else about the capsule is wrong, so a preview that admitted this would
    // report a size figure no member backs.
    entry.bytes = payload.size() + 1;
    manifest.files = {entry};

    // Assign the identity after the rows are final, exactly as the exporter
    // does, so the capsule is self-consistent apart from the size claim.
    manifest.revision_id = digest_or_fail(manifest);
    const std::string manifest_json = canonical_or_fail(manifest);

    std::vector<WriteMember> members;
    members.push_back(WriteMember{std::string(kManifestPath), bytes_of(manifest_json)});
    members.push_back(WriteMember{"payload.bin", payload});
    REQUIRE(write_archive_no_replace(members, destination).has_value());

    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());

    const ProfileRegistry registry = test_registry();
    AdmissionOptions options;
    options.product = "test-product";

    const auto preview = preview_capsule(*archive, registry, options);
    REQUIRE_FALSE(preview.has_value());
    CHECK(preview.error().status == CapsuleStatus::closure_violation);
    CHECK(preview.error().subject == "payload.bin");
    CHECK(preview.error().required == std::to_string(payload.size() + 1));
    CHECK(preview.error().found == std::to_string(payload.size()));

    // Control: the identical capsule with an honest size previews cleanly, so
    // the rejection is about the size claim and not about the hand-built
    // container.
    const fs::path honest_destination = temp.path() / "honest.capsule";
    Manifest honest = manifest;
    honest.files.front().bytes = payload.size();
    honest.revision_id = digest_or_fail(honest);
    std::vector<WriteMember> honest_members;
    honest_members.push_back(
        WriteMember{std::string(kManifestPath), bytes_of(canonical_or_fail(honest))});
    honest_members.push_back(WriteMember{"payload.bin", payload});
    REQUIRE(write_archive_no_replace(honest_members, honest_destination).has_value());
    auto honest_archive = open_archive(honest_destination);
    REQUIRE(honest_archive.has_value());
    CHECK(preview_capsule(*honest_archive, registry, options).has_value());
}

TEST_CASE("exporting identical content twice produces byte-identical files",
          "[authoring-capsule][determinism]") {
    TempDir temp;
    const fs::path first = temp.path() / "first.capsule";
    const fs::path second = temp.path() / "second.capsule";

    REQUIRE(export_capsule(make_export_request(), first).has_value());

    // The second request is built from scratch and lists its items in the
    // opposite order, so the comparison covers the exporter's canonical
    // ordering as well as its fixed metadata.
    ExportRequest reordered = make_export_request();
    std::reverse(reordered.items.begin(), reordered.items.end());
    REQUIRE(export_capsule(std::move(reordered), second).has_value());

    const auto first_bytes = read_file(first);
    const auto second_bytes = read_file(second);
    REQUIRE_FALSE(first_bytes.empty());
    // No timestamps, no host paths, no insertion order: the same inventory is
    // the same file, which is what lets two machines agree a capsule is
    // unchanged.
    CHECK(first_bytes == second_bytes);

    // Writing over a name that already exists is refused rather than resolved.
    const auto conflict = export_capsule(make_export_request(), first);
    REQUIRE_FALSE(conflict.has_value());
    CHECK(conflict.error().status == CapsuleStatus::publication_conflict);
}

TEST_CASE("revision identity survives a change of container compression",
          "[authoring-capsule][determinism]") {
    TempDir temp;
    const fs::path deflated_path = temp.path() / "deflated.capsule";
    const fs::path stored_path = temp.path() / "stored.capsule";

    ExportRequest request;
    request.manifest = base_manifest();
    request.manifest.files.clear();
    request.manifest.dependencies.clear();

    ExportItem item;
    item.entry = file_row("audio/render.pcm", "audio.rendition");
    item.entry.media_type = "application/octet-stream";
    item.bytes = compressible_payload(100000);
    request.items = {item};

    // The writer deflates when it has working-set room and stores when it does
    // not, so lowering only that budget changes the container encoding while
    // leaving the logical content identical. That is the axis the format
    // promises the digest does not see.
    CapsuleLimits tight = kCapsuleLimitsV1;
    tight.max_working_set_bytes = 150000;

    REQUIRE(export_capsule(request, deflated_path).has_value());
    REQUIRE(export_capsule(request, stored_path, tight).has_value());

    auto deflated = open_archive(deflated_path);
    auto stored = open_archive(stored_path);
    REQUIRE(deflated.has_value());
    REQUIRE(stored.has_value());

    // The premise of the test, asserted rather than assumed: the two
    // containers really did encode the payload differently.
    REQUIRE(deflated->members().size() == 2);
    REQUIRE(stored->members().size() == 2);
    CHECK(deflated->members()[1].method == 8);
    CHECK(stored->members()[1].method == 0);
    CHECK(deflated->members()[1].compressed_bytes < stored->members()[1].compressed_bytes);
    CHECK(read_file(deflated_path) != read_file(stored_path));

    const auto from_deflated = parse_manifest(std::string_view(
        reinterpret_cast<const char*>(deflated->manifest_bytes().data()),
        deflated->manifest_bytes().size()));
    const auto from_stored = parse_manifest(std::string_view(
        reinterpret_cast<const char*>(stored->manifest_bytes().data()),
        stored->manifest_bytes().size()));
    REQUIRE(from_deflated.has_value());
    REQUIRE(from_stored.has_value());

    CHECK(from_deflated->revision_id == from_stored->revision_id);
    CHECK(digest_or_fail(*from_deflated) == digest_or_fail(*from_stored));
    CHECK(canonical_or_fail(*from_deflated) == canonical_or_fail(*from_stored));
}

// ── Fail-closed behaviour found by adversarial review ───────────────────────
//
// Each of these covers a check that was declared in the contract and absent
// from the code. They are grouped because they share one failure mode: a
// promise that reads as enforced while nothing enforces it.

TEST_CASE("self_contained cannot be forged by omitting required_for",
          "[authoring-capsule][completeness][rights]") {
    // `required_for` is optional, so a manifest can leave it off every row. A
    // derivation that only inspected gating rows would then touch nothing and
    // fall through to its initial `true`, reporting a capsule full of
    // unknown-rights components as "every required byte is present and may be
    // redistributed" — while the same preview lists those rows as blocking.
    Manifest manifest = base_manifest();
    manifest.dependencies.clear();
    manifest.files.clear();

    FileEntry ungated = file_row("dsp/main.cpp");
    ungated.policy.required_for.clear();
    ungated.policy.redistribution = Redistribution::unknown();
    manifest.files.push_back(ungated);

    CHECK(derive_completeness(manifest) != Completeness::self_contained);

    // The same row with an explicit grant is fine: it is the unknown rights
    // that disqualify it, not the missing gate.
    manifest.files.front().policy.redistribution = Redistribution::granted();
    CHECK(derive_completeness(manifest) == Completeness::self_contained);

    // `restricted` is disqualifying for the same reason, and neither ever
    // decays to `allowed`.
    manifest.files.front().policy.redistribution = Redistribution::restricted();
    CHECK(derive_completeness(manifest) != Completeness::self_contained);
}

TEST_CASE("a profile's required role must actually be present",
          "[authoring-capsule][preview][roles]") {
    TempDir temp;
    const fs::path destination = temp.path() / "no-role.capsule";

    // TestProfileValidator requires "dsp.source". Ship a capsule that carries
    // only a rendition, so the role the profile published is missing.
    ExportRequest request;
    request.manifest = base_manifest();
    request.manifest.files.clear();
    request.manifest.dependencies.clear();

    ExportItem audio;
    audio.entry = file_row("audio/render.pcm", "audio.rendition");
    audio.entry.media_type = "audio/x-pulp-canonical-pcm";
    audio.entry.policy = included_policy(Redistribution::granted(), {RequiredFor::play});
    audio.bytes = to_canonical_bytes(make_pcm(1, 48000, {0.0f, 0.25f}));
    request.items = {audio};

    REQUIRE(export_capsule(request, destination).has_value());
    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());

    AdmissionOptions options;
    options.product = "test-product";
    const auto preview = preview_capsule(*archive, test_registry(), options);
    REQUIRE(preview.has_value());

    CHECK(preview->compatibility == CompatibilityVerdict::unsupported);
    CHECK(preview->unmet.status == CapsuleStatus::missing_required_role);
    CHECK(preview->unmet.subject == "dsp.source");

    // And the verdict has to bite: extraction must refuse rather than hand an
    // unsatisfiable capsule to the validator.
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    auto admitted = admit_to_staging(*archive, *preview, test_registry(), *staging);
    REQUIRE_FALSE(admitted.has_value());
    CHECK(admitted.error().status == CapsuleStatus::missing_required_role);
}

TEST_CASE("admit_to_staging refuses an unsupported capability",
          "[authoring-capsule][preview][fail-closed]") {
    TempDir temp;
    const fs::path destination = temp.path() / "needs-gpu.capsule";

    ExportRequest request = make_export_request();
    request.manifest.required_capabilities = {"gpu-nam"};

    REQUIRE(export_capsule(request, destination).has_value());
    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());

    AdmissionOptions options;
    options.product = "test-product";
    const auto preview = preview_capsule(*archive, test_registry(), options);

    // Preview itself succeeds — it exists so a product can say what is
    // missing — but records the capability as unavailable.
    REQUIRE(preview.has_value());
    CHECK(preview->compatibility == CompatibilityVerdict::unsupported);
    CHECK(preview->unmet.status == CapsuleStatus::unsupported_capability);
    CHECK(preview->unmet.subject == "gpu-nam");

    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    auto admitted = admit_to_staging(*archive, *preview, test_registry(), *staging);
    REQUIRE_FALSE(admitted.has_value());
    CHECK(admitted.error().status == CapsuleStatus::unsupported_capability);

    // Nothing was extracted: a refusal must not leave the capsule's contents
    // sitting in staging for a caller that ignored the error.
    CHECK(fs::is_empty(staging->root()));
}

TEST_CASE("an attestation naming a different digest is refused",
          "[authoring-capsule][preview][trust]") {
    TempDir temp;
    const fs::path destination = temp.path() / "wrong-digest.capsule";

    // An envelope lifted wholesale from another capsule: well-formed, real
    // signer, decodable signature, but it names a digest these bytes do not
    // produce. Without this check it would reach a consumer's verifier looking
    // entirely ordinary, and a verifier that trusts the field over the computed
    // digest would accept it.
    ExportRequest request = make_export_request();
    request.manifest.attestations_json =
        R"([{"algorithm":"ed25519","signature":"AAAA","signer_id":"someone",)"
        R"("signed_payload_digest":"sha256:)" +
        std::string(64, 'b') + R"("}])";

    REQUIRE(export_capsule(request, destination).has_value());
    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());

    AdmissionOptions options;
    options.product = "test-product";

    // No verifier is configured, so this refusal comes from the substrate
    // itself rather than from an adapter that might not look.
    const auto preview = preview_capsule(*archive, test_registry(), options);
    REQUIRE_FALSE(preview.has_value());
    CHECK(preview.error().status == CapsuleStatus::signature_invalid);
    CHECK(preview.error().subject == "someone");
}

TEST_CASE("reserved device names are rejected in every documented form",
          "[authoring-capsule][safe-path]") {
    // Windows resolves these as devices wherever they appear, so a member named
    // CON opens a device rather than a file. They are refused on every platform:
    // a capsule that cannot be extracted on one is a capsule that does not
    // travel, and the recipient's machine is the wrong place to discover that.
    for (const char* name : {"CON", "PRN", "AUX", "NUL", "COM1", "COM9", "LPT1", "LPT9",
                             "con", "Com1", "CON.txt", "com1.pcm"}) {
        auto rejected = admit_member_path(name);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().status == CapsuleStatus::path_rejected);
        CHECK(rejected.error().required == "no-reserved-device-name");
    }

    // Longer than the four characters a naive length check allows, which is
    // exactly why these are the ones that get missed.
    for (const char* name : {"CLOCK$", "CONIN$", "CONOUT$", "conout$"}) {
        auto rejected = admit_member_path(name);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().required == "no-reserved-device-name");
    }

    // Windows also accepts the SUPERSCRIPT digits as COM1/COM2/COM3. Three
    // UTF-8 bytes wide, so a byte-oriented check sees an ordinary name. These
    // are refused today by the NFC allowlist, which runs first — assert only
    // that they are refused, not which rule caught them, because the point is
    // that both layers cover it and neither may be the only one.
    for (const char* name : {"COM\xC2\xB9", "COM\xC2\xB2", "LPT\xC2\xB3"}) {
        CHECK_FALSE(admit_member_path(name).has_value());
    }

    // Names that merely start like a device are ordinary files.
    for (const char* name : {"console.json", "com0", "com10", "auxiliary.wav", "nulled"}) {
        INFO(name);
        CHECK(admit_member_path(name).has_value());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Status token vocabulary
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// The last enumerator of `CapsuleStatus`, and therefore the top of the range
/// the token contract covers. Pinned as a named constant because the checks
/// below sweep `[0, kLastCapsuleStatus]` as "every status that exists" and
/// treat everything above it as "not a status", which is only true while this
/// really is the final enumerator.
constexpr CapsuleStatus kLastCapsuleStatus = CapsuleStatus::cancelled;

/// The token `status_token()` returns for a value that is not a named
/// enumerator. It deliberately collides with the token for
/// `manifest_invalid`, which is what makes a fall-through detectable: an
/// enumerator that lost its case stops being distinguishable from the
/// most generic rejection there is.
constexpr std::string_view kStatusFallbackToken = "manifest_invalid";

/// The token every enumerator is contracted to produce.
///
/// Written as an exhaustive `switch` with no `default` label on purpose: a
/// new `CapsuleStatus` enumerator makes the compiler point at this function,
/// so the value gets an expectation here at the same moment it gets a case in
/// `status_token()`. A table keyed by integer value would compile silently and
/// simply never mention the new status.
///
/// Restating the strings is the mechanism, not redundancy. These tokens are a
/// published contract that products key their user-facing copy off, so a
/// rename inside `status_token()` is a breaking change that must fail here
/// rather than pass because both copies moved together.
std::string_view expected_status_token(CapsuleStatus status) {
    switch (status) {
        case CapsuleStatus::ok: return "ok";
        case CapsuleStatus::manifest_invalid: return "manifest_invalid";
        case CapsuleStatus::manifest_not_first: return "manifest_not_first";
        case CapsuleStatus::unsupported_format: return "unsupported_format";
        case CapsuleStatus::unsupported_format_version: return "unsupported_format_version";
        case CapsuleStatus::unsupported_profile: return "unsupported_profile";
        case CapsuleStatus::unsupported_profile_version: return "unsupported_profile_version";
        case CapsuleStatus::unsupported_product: return "unsupported_product";
        case CapsuleStatus::unsupported_capability: return "unsupported_capability";
        case CapsuleStatus::missing_required_role: return "missing_required_role";
        case CapsuleStatus::runtime_floor_too_old: return "runtime_floor_too_old";
        case CapsuleStatus::schema_migration_refused: return "schema_migration_refused";
        case CapsuleStatus::closure_violation: return "closure_violation";
        case CapsuleStatus::digest_mismatch: return "digest_mismatch";
        case CapsuleStatus::unsafe_archive: return "unsafe_archive";
        case CapsuleStatus::archive_budget_exceeded: return "archive_budget_exceeded";
        case CapsuleStatus::path_rejected: return "path_rejected";
        case CapsuleStatus::path_collision: return "path_collision";
        case CapsuleStatus::missing_dependency: return "missing_dependency";
        case CapsuleStatus::dependency_digest_mismatch: return "dependency_digest_mismatch";
        case CapsuleStatus::dependency_provider_denied: return "dependency_provider_denied";
        case CapsuleStatus::missing_licensed_sample: return "missing_licensed_sample";
        case CapsuleStatus::rights_insufficient: return "rights_insufficient";
        case CapsuleStatus::signature_invalid: return "signature_invalid";
        case CapsuleStatus::revoked_signer: return "revoked_signer";
        case CapsuleStatus::downgrade_refused: return "downgrade_refused";
        case CapsuleStatus::creator_identity_required: return "creator_identity_required";
        case CapsuleStatus::decode_unsupported: return "decode_unsupported";
        case CapsuleStatus::staging_failed: return "staging_failed";
        case CapsuleStatus::publication_conflict: return "publication_conflict";
        case CapsuleStatus::cancelled: return "cancelled";
    }
    // Unreachable for a named enumerator. Empty rather than a plausible token
    // so a value that escapes the switch fails the non-empty check below
    // instead of quietly matching something.
    return {};
}

}  // namespace

TEST_CASE("status_token gives every status a non-empty, unique, stable token",
          "[authoring-capsule][status]") {
    const auto last = static_cast<unsigned>(kLastCapsuleStatus);

    // Sweep the whole enumerator range rather than a hand-listed set. A
    // hand-written table alone would keep passing when a new enumerator falls
    // through the switch, because the table would simply never mention it.
    std::vector<std::string> tokens;
    tokens.reserve(last + 1);
    for (unsigned value = 0; value <= last; ++value) {
        const auto status = static_cast<CapsuleStatus>(value);
        const std::string token(status_token(status));
        INFO("enumerator value " << value << " token '" << token << "'");
        CHECK_FALSE(token.empty());
        // The published spelling, so a rename inside status.cpp breaks a
        // consumer's copy here rather than in the consumer's product.
        CHECK(token == expected_status_token(status));
        tokens.push_back(token);
    }

    // Control: the sweep actually visited the taxonomy rather than an empty or
    // truncated range. `ok` plus the thirty documented rejections.
    REQUIRE(tokens.size() == 31);

    // Uniqueness is what catches a fall-through: a status whose case was lost
    // returns the fallback and becomes indistinguishable from
    // `manifest_invalid`, so a product reporting it would tell the user the
    // wrong thing while nothing here failed.
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        for (std::size_t j = i + 1; j < tokens.size(); ++j) {
            INFO("enumerator values " << i << " and " << j << " share token '" << tokens[i] << "'");
            CHECK(tokens[i] != tokens[j]);
        }
    }
}

TEST_CASE("status_token yields the fallback for every value that is not a status",
          "[authoring-capsule][status]") {
    // `CapsuleStatus` has a fixed underlying type, so every value of that type
    // is a well-formed enum value and a caller can hand one over — from a
    // corrupted read, or a wire field a newer writer produced. The token must
    // still be safe to print and embed, so it may never be empty.
    //
    // This doubles as the tripwire for `kLastCapsuleStatus` going stale: if a
    // future enumerator is appended past it *and* wired up in status.cpp, that
    // value stops returning the fallback and this fails, which is the prompt to
    // widen the swept range above.
    for (unsigned value = static_cast<unsigned>(kLastCapsuleStatus) + 1;
         value <= std::numeric_limits<std::uint8_t>::max(); ++value) {
        const auto token = status_token(static_cast<CapsuleStatus>(value));
        INFO("unnamed value " << value);
        CHECK_FALSE(token.empty());
        CHECK(token == kStatusFallbackToken);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Profile registry
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// A validator whose identifier and version are set per instance, so a test
/// can register several — including two claiming the same identifier — and
/// tell from a lookup which instance actually answered.
class RegistryProbeValidator final : public ProfileValidator {
public:
    RegistryProbeValidator(std::string id, std::uint32_t version)
        : id_(std::move(id)), version_(version) {}

    std::string_view profile_id() const noexcept override { return id_; }
    std::uint32_t max_profile_version() const noexcept override { return version_; }
    std::vector<std::string> required_roles() const override { return {}; }
    bool supports_capability(std::string_view) const noexcept override { return false; }
    pulp::runtime::Result<void, CapsuleError> check_compatibility(const Manifest&) const override {
        return {};
    }
    pulp::runtime::Result<void, CapsuleError> validate_staged(const Manifest&,
                                                              const fs::path&) const override {
        return {};
    }

private:
    std::string id_;
    std::uint32_t version_;
};

std::shared_ptr<RegistryProbeValidator> registry_probe(std::string id, std::uint32_t version = 1) {
    return std::make_shared<RegistryProbeValidator>(std::move(id), version);
}

}  // namespace

TEST_CASE("the profile registry answers for exactly what was registered",
          "[authoring-capsule][profile-registry]") {
    ProfileRegistry registry;
    registry.register_profile(registry_probe("com.example.alpha", 1));
    registry.register_profile(registry_probe("com.example.beta", 7));

    // Each registration is retrievable, and the version proves the lookup
    // returned *that* validator rather than whichever one the map happened to
    // hold first.
    const auto* alpha = registry.find("com.example.alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->profile_id() == "com.example.alpha");
    CHECK(alpha->max_profile_version() == 1u);

    const auto* beta = registry.find("com.example.beta");
    REQUIRE(beta != nullptr);
    CHECK(beta->profile_id() == "com.example.beta");
    CHECK(beta->max_profile_version() == 7u);

    CHECK(alpha != beta);

    // An unregistered identifier is null, not a near-match. A registry that
    // substituted a prefix or a shared namespace would hand a capsule to a
    // validator that was never asked to speak for it, and the product would
    // lose the exact identifier it needs to name the missing download.
    CHECK(registry.find("com.example.gamma") == nullptr);
    CHECK(registry.find("com.example.alph") == nullptr);
    CHECK(registry.find("com.example.alphaa") == nullptr);
    CHECK(registry.find("com.example.") == nullptr);
    CHECK(registry.find("") == nullptr);

    const auto ids = registry.registered_profiles();
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "com.example.alpha");
    CHECK(ids[1] == "com.example.beta");
}

TEST_CASE("re-registering a profile identifier replaces it rather than duplicating it",
          "[authoring-capsule][profile-registry]") {
    ProfileRegistry registry;
    // Control: an unrelated profile that must survive both re-registrations,
    // so a registry that simply held one entry at a time could not pass.
    registry.register_profile(registry_probe("com.example.other", 3));

    registry.register_profile(registry_probe("com.example.alpha", 1));
    registry.register_profile(registry_probe("com.example.alpha", 9));

    const auto* alpha = registry.find("com.example.alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->max_profile_version() == 9u);

    // Descending on purpose: last-registration-wins, not highest-version-wins.
    // Both look identical when the replacement only ever goes up.
    registry.register_profile(registry_probe("com.example.alpha", 2));
    alpha = registry.find("com.example.alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->max_profile_version() == 2u);

    const auto* other = registry.find("com.example.other");
    REQUIRE(other != nullptr);
    CHECK(other->max_profile_version() == 3u);

    // One row per identifier: a duplicate would make which validator answers
    // depend on registration order.
    const auto ids = registry.registered_profiles();
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "com.example.alpha");
    CHECK(ids[1] == "com.example.other");
}

TEST_CASE("registered_profiles is reproducible regardless of registration order",
          "[authoring-capsule][profile-registry]") {
    // The listing is a diagnostic two runs are meant to be able to compare, so
    // it must not carry the arrival order of the registrations.
    ProfileRegistry ascending;
    for (const char* id : {"com.example.a", "com.example.m", "com.example.z"})
        ascending.register_profile(registry_probe(id));

    ProfileRegistry shuffled;
    for (const char* id : {"com.example.m", "com.example.z", "com.example.a"})
        shuffled.register_profile(registry_probe(id));

    const auto from_ascending = ascending.registered_profiles();
    const auto from_shuffled = shuffled.registered_profiles();

    // Control: both registries really hold all three, so an implementation
    // that dropped entries could not pass by returning two empty lists.
    REQUIRE(from_ascending.size() == 3);
    REQUIRE(from_shuffled.size() == 3);

    CHECK(from_ascending == from_shuffled);
    CHECK(std::is_sorted(from_ascending.begin(), from_ascending.end()));

    const std::vector<std::string> expected{"com.example.a", "com.example.m", "com.example.z"};
    CHECK(from_ascending == expected);
}

TEST_CASE("a validator with no identity is refused rather than stored",
          "[authoring-capsule][profile-registry]") {
    ProfileRegistry registry;

    // A null validator has nothing to key on and an empty identifier is not an
    // identifier. Both are dropped rather than dereferenced or filed under "":
    // a registry that crashed on a redundant registration, or that answered a
    // lookup for the empty string, would be worse than one reporting absence.
    registry.register_profile(nullptr);
    registry.register_profile(registry_probe("", 4));

    CHECK(registry.registered_profiles().empty());
    CHECK(registry.find("") == nullptr);

    // Control: the refusals left the registry usable. Without this, an
    // implementation that simply stopped accepting registrations after the
    // first bad one would pass every check above.
    registry.register_profile(registry_probe("com.example.alpha", 5));
    const auto* alpha = registry.find("com.example.alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->max_profile_version() == 5u);

    const auto ids = registry.registered_profiles();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "com.example.alpha");
}

TEST_CASE("a moved-from profile registry reports absence instead of dereferencing",
          "[authoring-capsule][profile-registry]") {
    // A moved-from registry holds no map. That state is reachable from
    // ordinary consumer code — a registry built by a factory and returned by
    // value, or reassigned — so the contract is that it answers "nothing is
    // registered" and stays inert, never that it faults.
    ProfileRegistry source;
    source.register_profile(registry_probe("com.example.alpha", 1));

    SECTION("move construction") {
        ProfileRegistry moved_to(std::move(source));

        // The registrations went with the move, which is the control: a
        // registry that lost them would make the emptiness assertions below
        // pass for the wrong reason.
        const auto* alpha = moved_to.find("com.example.alpha");
        REQUIRE(alpha != nullptr);
        CHECK(alpha->max_profile_version() == 1u);
        CHECK(moved_to.registered_profiles().size() == 1);

        CHECK(source.find("com.example.alpha") == nullptr);  // NOLINT(bugprone-use-after-move)
        CHECK(source.registered_profiles().empty());

        // Registering into the emptied registry is ignored, not a crash, and
        // does not resurrect it into something a lookup can find.
        source.register_profile(registry_probe("com.example.beta", 2));
        CHECK(source.find("com.example.beta") == nullptr);
        CHECK(source.registered_profiles().empty());

        // And the ignored registration did not leak into the registry that
        // owns the map now.
        CHECK(moved_to.find("com.example.beta") == nullptr);
    }

    SECTION("move assignment") {
        ProfileRegistry target;
        target.register_profile(registry_probe("com.example.replaced", 8));
        target = std::move(source);

        // The assignment replaced the target's contents rather than merging.
        CHECK(target.find("com.example.replaced") == nullptr);
        const auto* alpha = target.find("com.example.alpha");
        REQUIRE(alpha != nullptr);
        CHECK(alpha->max_profile_version() == 1u);

        CHECK(source.find("com.example.alpha") == nullptr);  // NOLINT(bugprone-use-after-move)
        CHECK(source.registered_profiles().empty());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Path admission — UTF-8 strictness and the NFC-verifiable subset
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// UTF-8 encode one code point, including the forms the module's decoder must
/// refuse. Deliberately hand-written rather than routed through the module's
/// own encoder: a test that shared the encoder would cancel out exactly the
/// bug it is looking for.
std::string path_cp_utf8(char32_t cp) {
    const auto value = static_cast<std::uint32_t>(cp);
    std::string out;
    if (value < 0x80) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (value >> 6)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (value >> 12)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (value >> 18)));
        out.push_back(static_cast<char>(0x80U | ((value >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
    return out;
}

/// A member path whose only interesting character is `cp`, placed inside a
/// component so that no positional rule — leading separator, trailing dot,
/// device name — can answer before the character class does.
std::string path_with_cp(char32_t cp) { return "audio/x" + path_cp_utf8(cp) + "y.bin"; }

constexpr std::string_view kRuleWellFormedUtf8 = "well-formed-utf8";
constexpr std::string_view kRuleNoColon = "no-colon";
constexpr std::string_view kRuleReservedCharacter = "no-reserved-character";

}  // namespace

TEST_CASE("admit_member_path refuses every non-shortest and out-of-range UTF-8 form",
          "[authoring-capsule][safe-path]") {
    // The control for the whole case. A decoder that refused all multi-byte
    // input would satisfy every rejection below without doing its job.
    expect_path_admitted("audio/caf\xC3\xA9.bin");    // U+00E9, shortest form
    expect_path_admitted("audio/\xE4\xB8\x80.bin");   // U+4E00, shortest form

    SECTION("overlong encodings") {
        // U+00E9 has exactly one shortest spelling. Its three- and four-byte
        // spellings decode to the same code point under a lenient decoder,
        // which is precisely how two distinct byte strings come to denote one
        // path — the aliasing this module exists to prevent.
        expect_path_rejected("audio/caf\xE0\x83\xA9.bin", kRuleWellFormedUtf8);
        expect_path_rejected("audio/caf\xF0\x80\x83\xA9.bin", kRuleWellFormedUtf8);
        // The overlong '/' is the classic traversal smuggler: it must fail as
        // malformed rather than decode into a separator that the component
        // pass would then have to catch.
        expect_path_rejected("audio\xC0\xAF..\xC0\xAFsecret.key", kRuleWellFormedUtf8);
        // The overlong NUL, which a lenient decoder turns into a C0 control.
        expect_path_rejected("audio/pay\xC0\x80load.bin", kRuleWellFormedUtf8);
    }

    SECTION("surrogate halves") {
        // UTF-16 surrogates are not code points. Admitting them would give a
        // supplementary character a second spelling.
        expect_path_rejected(path_with_cp(0xD800), kRuleWellFormedUtf8);
        expect_path_rejected(path_with_cp(0xDBFF), kRuleWellFormedUtf8);
        expect_path_rejected(path_with_cp(0xDC00), kRuleWellFormedUtf8);
        expect_path_rejected(path_with_cp(0xDFFF), kRuleWellFormedUtf8);
        // A CESU-8 surrogate pair spelling U+1F600.
        expect_path_rejected("audio/x" + path_cp_utf8(0xD83D) + path_cp_utf8(0xDE00) + "y.bin",
                             kRuleWellFormedUtf8);
        // The control that pins the surrogate guard's lower edge: U+D7A3 is
        // the last Hangul syllable and its lead byte is also 0xED, so a guard
        // written one code point too wide would refuse it.
        expect_path_admitted(path_with_cp(0xD7A3));
    }

    SECTION("beyond U+10FFFF") {
        // U+10FFFF is the last code point the decoder may produce. It decodes
        // and is then refused by the NFC subset, while U+110000 must not
        // decode at all — the pair puts the cap in exactly one place.
        expect_path_rejected(path_with_cp(0x10FFFF), kRuleNfcSubset);
        expect_path_rejected("audio/x\xF4\x90\x80\x80y.bin", kRuleWellFormedUtf8);
        expect_path_rejected("audio/x\xF5\x80\x80\x80y.bin", kRuleWellFormedUtf8);
        expect_path_rejected("audio/x\xF8\x88\x80\x80\x80y.bin", kRuleWellFormedUtf8);
        expect_path_rejected("audio/x\xFF\xFEy.bin", kRuleWellFormedUtf8);
    }

    SECTION("truncated and stray bytes") {
        expect_path_rejected("audio/x\xE4\xB8", kRuleWellFormedUtf8);        // cut short
        expect_path_rejected("audio/x\xE4\xB8/y.bin", kRuleWellFormedUtf8);  // cut by a separator
        expect_path_rejected("audio/\x80xy.bin", kRuleWellFormedUtf8);       // leading continuation
        expect_path_rejected("audio/x\xC1\x81y.bin", kRuleWellFormedUtf8);   // C1 lead byte
    }
}

TEST_CASE("the NFC-verifiable allowlist admits each range to its exact edges",
          "[authoring-capsule][safe-path]") {
    struct Range {
        char32_t first;
        char32_t last;
        const char* what;
    };
    // Both edges of every admitted range. Testing the interior only would let
    // an off-by-one in the allowlist through, and the ranges abut characters
    // that must stay out.
    static constexpr Range kRanges[] = {
        {0x0020, 0x007E, "ASCII graphic and space"},
        {0x00C0, 0x00D6, "Latin-1 letters below the multiplication sign"},
        {0x00D8, 0x00F6, "Latin-1 letters between the two signs"},
        {0x00F8, 0x00FF, "Latin-1 letters above the division sign"},
        {0x0100, 0x017F, "Latin Extended-A"},
        {0x3041, 0x3096, "Hiragana"},
        {0x30A1, 0x30FA, "Katakana"},
        {0x30FC, 0x30FC, "Katakana-Hiragana prolonged sound mark"},
        {0x3400, 0x4DBF, "CJK unified extension A"},
        {0x4E00, 0x9FFF, "CJK unified"},
        {0xAC00, 0xD7A3, "precomposed Hangul syllables"},
    };
    for (const auto& range : kRanges) {
        INFO(range.what);
        expect_path_admitted(path_with_cp(range.first));
        expect_path_admitted(path_with_cp(range.last));
    }

    // One code point outside each edge. This is what turns the list above from
    // "some characters are accepted" into a boundary: the multiplication and
    // division signs are the two holes punched in Latin-1, and the katakana
    // middle dot sits between the syllables and the prolonged sound mark.
    for (std::uint32_t cp : {0x00BFu, 0x00D7u, 0x00F7u, 0x0180u, 0x3040u, 0x3097u, 0x30A0u,
                             0x30FBu, 0x30FDu, 0x33FFu, 0x4DC0u, 0x4DFFu, 0xA000u, 0xABFFu,
                             0xD7A4u}) {
        INFO(cp);
        expect_path_rejected(path_with_cp(static_cast<char32_t>(cp)), kRuleNfcSubset);
    }
}

TEST_CASE("the NFC-verifiable subset refuses everything it cannot prove composed",
          "[authoring-capsule][safe-path]") {
    SECTION("a decomposed sequence, against its precomposed control") {
        // The rule is "prove NFC", not "refuse non-ASCII": the precomposed
        // spelling of the same text is admitted, and only the spelling that
        // would need a normalizer is refused.
        expect_path_admitted("audio/f" + path_cp_utf8(0x00FC) + "r.bin");
        expect_path_rejected("audio/fu" + path_cp_utf8(0x0308) + "r.bin", kRuleNfcSubset);
        expect_path_admitted("audio/" + path_cp_utf8(0x00C5) + "ngstrom.bin");
        expect_path_rejected("audio/A" + path_cp_utf8(0x030A) + "ngstrom.bin", kRuleNfcSubset);
    }

    SECTION("combining marks") {
        // Canonical reordering moves combining marks and canonical composition
        // absorbs them. Neither operation is available here, so no code point
        // that either could act on may be admitted.
        for (std::uint32_t cp : {0x0300u, 0x0301u, 0x0327u, 0x0345u, 0x20E0u}) {
            INFO(cp);
            expect_path_rejected(path_with_cp(static_cast<char32_t>(cp)), kRuleNfcSubset);
        }
    }

    SECTION("CJK compatibility ideographs") {
        // The compatibility block sits outside the allowlist wholesale: the
        // canonical singletons in it are NFC_QC=No, and vetting the block
        // character by character is exactly the work this file cannot do.
        expect_path_admitted(path_with_cp(0x4E00));  // the unified control
        for (std::uint32_t cp : {0xF900u, 0xF9FFu, 0xFA30u, 0xFAD9u}) {
            INFO(cp);
            expect_path_rejected(path_with_cp(static_cast<char32_t>(cp)), kRuleNfcSubset);
        }
    }

    SECTION("Hangul conjoining jamo") {
        // Jamo compose into the precomposed syllables that are admitted, so
        // admitting both spellings would give one name two byte strings.
        expect_path_admitted(path_with_cp(0xAC00));
        expect_path_rejected(path_with_cp(0x1100), kRuleNfcSubset);
        expect_path_rejected(path_with_cp(0x1161), kRuleNfcSubset);
        expect_path_rejected(path_with_cp(0x11A8), kRuleNfcSubset);
    }

    SECTION("supplementary planes") {
        // Including a CJK ideograph whose Basic-Plane siblings are admitted:
        // the boundary is the vetted range, not the script.
        for (std::uint32_t cp : {0x1F600u, 0x20000u, 0x2F800u, 0x10FFFFu}) {
            INFO(cp);
            expect_path_rejected(path_with_cp(static_cast<char32_t>(cp)), kRuleNfcSubset);
        }
    }
}

TEST_CASE("admit_member_path names the colon and the Windows-reserved characters",
          "[authoring-capsule][safe-path]") {
    SECTION("colon") {
        expect_path_rejected("audio/pay:load.bin", kRuleNoColon);
        // Second byte is a colon but the first is not a letter, so the
        // drive-letter shape does not apply and the colon rule must answer.
        expect_path_rejected("1:payload.bin", kRuleNoColon);
        // An NTFS alternate data stream, which names a different byte stream
        // inside the same file.
        expect_path_rejected("audio/payload.bin:x", kRuleNoColon);
        // The control that keeps the two rules distinguishable: only the
        // drive-letter shape reports the drive-letter token.
        expect_path_rejected("C:/Windows/x.dll", kRuleDriveLetter);
    }

    SECTION("reserved characters") {
        for (const char* raw : {"audio/pay<load.bin", "audio/pay>load.bin",
                                "audio/pay\"load.bin", "audio/pay|load.bin",
                                "audio/pay?load.bin", "audio/pay*load.bin"}) {
            INFO(raw);
            expect_path_rejected(raw, kRuleReservedCharacter);
        }
        // The control: this is a named set, not a blanket ban on punctuation.
        expect_path_admitted("audio/pay(load)[1]{x}~!#$%&'+,-;=@^_.bin");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Collision key folding
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("collision_key folds every Latin Extended-A case run",
          "[authoring-capsule][safe-path]") {
    // The block alternates upper and lower, but the parity of the pair flips
    // twice across it. A single "even is uppercase" rule folds half the block
    // onto the wrong letter, so each run is pinned at both of its ends.
    struct Pair {
        char32_t upper;
        char32_t lower;
    };
    static constexpr Pair kPairs[] = {
        {0x0100, 0x0101}, {0x0136, 0x0137},  // first run: even is uppercase
        {0x0139, 0x013A}, {0x0147, 0x0148},  // second run: odd is uppercase
        {0x014A, 0x014B}, {0x0176, 0x0177},  // third run: even again
        {0x0179, 0x017A}, {0x017D, 0x017E},  // fourth run: odd again
    };

    for (const auto& pair : kPairs) {
        INFO(static_cast<std::uint32_t>(pair.upper));
        const std::string upper = path_with_cp(pair.upper);
        const std::string lower = path_with_cp(pair.lower);
        // Both spellings must be admissible, or the fold would be describing
        // paths no capsule can carry.
        REQUIRE(admit_member_path(upper).has_value());
        REQUIRE(admit_member_path(lower).has_value());
        CHECK(collision_key(upper) == collision_key(lower));

        const auto checked = check_collisions({upper, lower});
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().status == CapsuleStatus::path_collision);
        CHECK(checked.error().subject == upper);
        CHECK(checked.error().found == lower);
    }

    // The control that makes those folds mean something. A run shifted by one,
    // or a fold that collapsed the block, would report these as colliding.
    CHECK(collision_key(path_with_cp(0x0100)) != collision_key(path_with_cp(0x0102)));
    CHECK(collision_key(path_with_cp(0x0139)) != collision_key(path_with_cp(0x013B)));
    CHECK(collision_key(path_with_cp(0x014A)) != collision_key(path_with_cp(0x014C)));
    CHECK(check_collisions({path_with_cp(0x0100), path_with_cp(0x0102), path_with_cp(0x0139),
                            path_with_cp(0x013B), path_with_cp(0x014A), path_with_cp(0x014C)})
              .has_value());
}

TEST_CASE("collision_key folds U+0178 out of its own block", "[authoring-capsule][safe-path]") {
    // Ÿ is the one Latin Extended-A capital whose lowercase lives back in
    // Latin-1. A fold that stayed inside the block would send it to U+0179.
    const std::string upper = path_with_cp(0x0178);
    const std::string lower = path_with_cp(0x00FF);
    REQUIRE(admit_member_path(upper).has_value());
    REQUIRE(admit_member_path(lower).has_value());

    CHECK(collision_key(upper) == collision_key(lower));
    CHECK(collision_key(upper) != collision_key(path_with_cp(0x0179)));
    CHECK(collision_key(upper) != collision_key(path_with_cp(U'y')));

    const auto checked = check_collisions({upper, lower});
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().status == CapsuleStatus::path_collision);
    CHECK(checked.error().subject == upper);
    CHECK(checked.error().found == lower);
}

TEST_CASE("collision_key maps each documented confusable to its representative",
          "[authoring-capsule][safe-path]") {
    struct Case {
        char32_t input;
        char32_t representative;
        const char* note;
    };
    static constexpr Case kCases[] = {
        {U'0', U'o', "digit zero reads as o"},
        {U'O', U'o', "capital O reaches o through case folding"},
        {U'l', U'i', "lowercase L reads as i"},
        {U'1', U'i', "digit one reads as i"},
        {U'L', U'i', "capital L folds to l, then to i"},
        {U'I', U'i', "capital I folds to i"},
        {0x0131, U'i', "dotless i"},
        {0x0130, U'i', "capital dotted I folds to dotless i, then to i"},
        {0x017F, U's', "long s"},
        {0x0138, U'k', "kra"},
    };

    for (const auto& entry : kCases) {
        INFO(entry.note);
        const std::string input = path_with_cp(entry.input);
        const std::string representative = path_with_cp(entry.representative);
        // A fold that named a character no path may carry would be unreachable
        // in practice, so each input is checked to be admissible first.
        REQUIRE(admit_member_path(input).has_value());
        CHECK(collision_key(input) == collision_key(representative));

        const auto checked = check_collisions({representative, input});
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().status == CapsuleStatus::path_collision);
        CHECK(checked.error().subject == representative);
        CHECK(checked.error().found == input);
    }

    // Folding two names together only ever costs a false collision, so the
    // failure this control guards against is the opposite one: a fold that
    // collapsed everything would make every case above pass for free.
    CHECK(check_collisions({path_with_cp(U'o'), path_with_cp(U'i'), path_with_cp(U's'),
                            path_with_cp(U'k')})
              .has_value());
    // Characters the table deliberately leaves alone.
    CHECK(collision_key(path_with_cp(U'5')) != collision_key(path_with_cp(U's')));
    CHECK(collision_key(path_with_cp(U'2')) != collision_key(path_with_cp(U'z')));
    CHECK(collision_key(path_with_cp(0x0142)) != collision_key(path_with_cp(U'i')));
}

TEST_CASE("collision_key keeps distinct inputs distinct outside the fold tables",
          "[authoring-capsule][safe-path]") {
    // A code point no fold table names comes back out unchanged, including a
    // supplementary-plane one that no admitted path can carry: collision_key
    // is total, and a re-encoder that dropped or truncated what it could not
    // fold would silently merge distinct members.
    // U+10FFFF is in the list because it is the only value that puts a
    // non-zero bit in a four-byte lead: an encoder that dropped the top bits
    // would round-trip every emoji unchanged and still be wrong.
    for (std::uint32_t cp : {0x00E0u, 0x3042u, 0x4E00u, 0xAC00u, 0x1F600u, 0x10FFFFu}) {
        INFO(cp);
        const std::string text = path_cp_utf8(static_cast<char32_t>(cp));
        CHECK(collision_key(text) == text);
    }

    // Malformed bytes cannot reach here from an admitted path, but the
    // documented behaviour is to copy them through rather than fold them onto
    // one replacement character, which would turn two unrelated names into a
    // collision the archive could not explain.
    CHECK(collision_key("\xFF" "a") == std::string("\xFF" "a"));
    CHECK(check_collisions({"\xFF" "a.bin", "\xFE" "a.bin"}).has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Source-container fixtures
//
// Every container below is assembled byte by byte rather than through a Pulp
// encoder. The decoder under test owns its own chunk reader, so a shared
// writer would let a bug in one cancel a bug in the other out.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

void append_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

struct IffChunkFixture {
    std::string id;
    std::vector<std::uint8_t> payload;
    /// Negative means "declare the payload's real length". A non-negative
    /// value is written into the size field instead, which is how a truncated
    /// or over-declaring container is built.
    std::int64_t declared_size = -1;
};

/// Assemble a RIFF (little-endian sizes) or IFF/AIFF FORM (big-endian sizes)
/// container. `form_size_override` is negative for a correct size field.
std::vector<std::uint8_t> build_iff_form(std::string_view form_id, std::string_view form_type,
                                         const std::vector<IffChunkFixture>& chunks,
                                         bool big_endian, std::int64_t form_size_override = -1) {
    std::vector<std::uint8_t> body;
    put_id(body, form_type);
    for (const auto& chunk : chunks) {
        put_id(body, chunk.id);
        const auto size = chunk.declared_size < 0
                              ? static_cast<std::uint32_t>(chunk.payload.size())
                              : static_cast<std::uint32_t>(chunk.declared_size);
        if (big_endian)
            append_be32(body, size);
        else
            put_u32(body, size);
        body.insert(body.end(), chunk.payload.begin(), chunk.payload.end());
        if (chunk.payload.size() % 2 != 0)
            body.push_back(0);
    }
    std::vector<std::uint8_t> out;
    put_id(out, form_id);
    const auto form_size = form_size_override < 0 ? static_cast<std::uint32_t>(body.size())
                                                  : static_cast<std::uint32_t>(form_size_override);
    if (big_endian)
        append_be32(out, form_size);
    else
        put_u32(out, form_size);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// The 16 mandatory bytes of a `fmt ` chunk. `block_align` is explicit so a
/// container can declare one that disagrees with its own frame size.
std::vector<std::uint8_t> wav_fmt_payload(std::uint16_t tag, std::uint16_t channels,
                                          std::uint32_t rate, std::uint16_t block_align,
                                          std::uint16_t bits) {
    std::vector<std::uint8_t> out;
    put_u16(out, tag);
    put_u16(out, channels);
    put_u32(out, rate);
    put_u32(out, rate * block_align);
    put_u16(out, block_align);
    put_u16(out, bits);
    return out;
}

/// A KSDATAFORMAT_SUBTYPE_* GUID: the format tag in the first two bytes, then
/// the twelve-byte tail every such subtype shares.
std::vector<std::uint8_t> ksdataformat_guid(std::uint16_t tag) {
    std::vector<std::uint8_t> guid;
    put_u16(guid, tag);
    put_u16(guid, 0);
    static constexpr std::uint8_t tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                             0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    guid.insert(guid.end(), std::begin(tail), std::end(tail));
    return guid;
}

/// A 40-byte WAVE_FORMAT_EXTENSIBLE `fmt ` payload wrapping `sub_tag`.
std::vector<std::uint8_t> wav_extensible_fmt_payload(std::uint16_t sub_tag, std::uint16_t channels,
                                                     std::uint32_t rate, std::uint16_t bits,
                                                     std::uint16_t valid_bits,
                                                     std::vector<std::uint8_t> guid) {
    const auto block_align = static_cast<std::uint16_t>(channels * (bits / 8));
    auto out = wav_fmt_payload(0xFFFE, channels, rate, block_align, bits);
    put_u16(out, 22);  // cbSize
    put_u16(out, valid_bits);
    put_u32(out, 0);  // channel mask
    out.insert(out.end(), guid.begin(), guid.end());
    return out;
}

/// The 80-bit extended-precision spelling of an exact integer sample rate.
std::vector<std::uint8_t> extended_rate_be(std::uint64_t value) {
    std::vector<std::uint8_t> out;
    if (value == 0) {
        out.assign(10, 0);
        return out;
    }
    int top = 63;
    while (((value >> top) & 1u) == 0)
        --top;
    const std::uint64_t mantissa = value << (63 - top);
    append_be16(out, static_cast<std::uint16_t>(16383 + top));
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((mantissa >> shift) & 0xFF));
    return out;
}

std::vector<std::uint8_t> aiff_comm_payload(std::uint16_t channels, std::uint32_t frames,
                                            std::uint16_t bits,
                                            const std::vector<std::uint8_t>& rate_bytes,
                                            std::string_view compression = {}) {
    std::vector<std::uint8_t> out;
    append_be16(out, channels);
    append_be32(out, frames);
    append_be16(out, bits);
    out.insert(out.end(), rate_bytes.begin(), rate_bytes.end());
    if (!compression.empty())
        put_id(out, compression);
    return out;
}

std::vector<std::uint8_t> aiff_ssnd_payload(std::uint32_t offset, std::uint32_t block_size,
                                            const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    append_be32(out, offset);
    append_be32(out, block_size);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::uint8_t> pcm16_be_data(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> out;
    for (const auto sample : samples)
        append_be16(out, static_cast<std::uint16_t>(sample));
    return out;
}

std::vector<std::uint8_t> float32_be_data(const std::vector<float>& samples) {
    std::vector<std::uint8_t> out;
    for (const auto sample : samples)
        append_be32(out, std::bit_cast<std::uint32_t>(sample));
    return out;
}

/// A canonical member assembled field by field, so a test can put a header in
/// it that `to_canonical_bytes()` would never produce.
std::vector<std::uint8_t> build_canonical_member(std::string_view magic, std::uint32_t version,
                                                 std::uint32_t channels, std::uint32_t rate,
                                                 std::uint64_t frame_count,
                                                 const std::vector<float>& samples) {
    std::vector<std::uint8_t> out;
    put_id(out, magic);
    put_u32(out, version);
    put_u32(out, channels);
    put_u32(out, rate);
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>((frame_count >> shift) & 0xFF));
    for (const auto sample : samples)
        put_u32(out, std::bit_cast<std::uint32_t>(sample));
    return out;
}

/// Assert a decode refusal names exactly the rule that caught it. A blanket
/// `decode_unsupported` would satisfy a test that only checked "this failed",
/// so the subject is what proves the decoder distinguished this shape.
void expect_decode_refused(const std::vector<std::uint8_t>& source, std::string_view subject,
                           std::string_view found = {}) {
    INFO(subject);
    const auto refused = decode_to_canonical(source);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().status == CapsuleStatus::decode_unsupported);
    CHECK(refused.error().subject == subject);
    if (!found.empty())
        CHECK(refused.error().found == found);
}

// ── FLAC construction ───────────────────────────────────────────────────────
//
// The FLAC path is the one admitted format whose decode this module delegates,
// so it needs a source that actually decodes rather than only a corrupt stream
// to refuse. The container is written by hand, like the WAV and AIFF fixtures
// above and for the same reason: a shared encoder would let one bug in the
// writer cancel out a bug in the reader.

/// A bit writer, most significant bit first — FLAC's own order.
class BitWriter {
public:
    void write(std::uint64_t value, int width) {
        for (int bit = width - 1; bit >= 0; --bit) {
            const auto next = static_cast<std::uint8_t>((value >> bit) & 1u);
            accumulator_ = static_cast<std::uint8_t>((accumulator_ << 1) | next);
            if (++held_ == 8) {
                bytes_.push_back(accumulator_);
                accumulator_ = 0;
                held_ = 0;
            }
        }
    }
    void pad_to_byte() {
        while (held_ != 0) write(0, 1);
    }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint8_t accumulator_ = 0;
    int held_ = 0;
};

/// FLAC's frame-header CRC: CRC-8 over polynomial x^8+x^2+x+1, zero seed.
std::uint8_t flac_crc8(const std::vector<std::uint8_t>& bytes) {
    std::uint8_t crc = 0;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint8_t>((crc & 0x80u) != 0 ? ((crc << 1) ^ 0x07u) : (crc << 1));
    }
    return crc;
}

/// FLAC's frame-footer CRC: CRC-16 over polynomial x^16+x^15+x^2+1, zero seed.
std::uint16_t flac_crc16(const std::vector<std::uint8_t>& bytes) {
    std::uint16_t crc = 0;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint16_t>(static_cast<std::uint16_t>(byte) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 0x8000u) != 0 ? ((crc << 1) ^ 0x8005u)
                                                                  : (crc << 1));
    }
    return crc;
}

/// A minimal 16-bit FLAC stream: a STREAMINFO block followed by one
/// fixed-blocksize frame whose subframes are VERBATIM, so the samples travel
/// unencoded and the fixture needs no entropy coder.
///
/// `declared_frames` is STREAMINFO's total-samples field. It defaults to what
/// the frame really carries; a truncation case raises it so the stream promises
/// more audio than it ships.
std::vector<std::uint8_t> make_flac(const std::vector<std::int16_t>& interleaved,
                                    std::uint32_t channels, std::uint32_t sample_rate,
                                    std::optional<std::uint64_t> declared_frames = std::nullopt) {
    const auto frames = static_cast<std::uint32_t>(interleaved.size() / channels);
    const std::uint64_t total = declared_frames.value_or(frames);

    std::vector<std::uint8_t> out{'f', 'L', 'a', 'C'};
    out.push_back(0x80);  // final metadata block, type 0 (STREAMINFO)
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x22);  // STREAMINFO is always 34 bytes

    BitWriter info;
    info.write(frames, 16);  // minimum block size
    info.write(frames, 16);  // maximum block size
    info.write(0, 24);       // minimum frame size, unknown
    info.write(0, 24);       // maximum frame size, unknown
    info.write(sample_rate, 20);
    info.write(channels - 1, 3);
    info.write(15, 5);  // bits per sample, less one
    info.write(total, 36);
    for (int i = 0; i < 16; ++i) info.write(0, 8);  // MD5 of the decoded audio, unknown
    out.insert(out.end(), info.bytes().begin(), info.bytes().end());

    BitWriter header;
    header.write(0x3FFEu, 14);  // frame sync
    header.write(0, 1);         // reserved
    header.write(0, 1);         // fixed block size, so the number below is a frame number
    header.write(0x7u, 4);      // block size follows the header as a 16-bit value, less one
    header.write(0x0u, 4);      // sample rate is the one STREAMINFO declared
    header.write(channels - 1, 4);
    header.write(0x4u, 3);  // 16 bits per sample
    header.write(0, 1);     // reserved
    header.write(0, 8);     // frame number zero, UTF-8 coded
    header.write(frames - 1, 16);
    header.write(flac_crc8(header.bytes()), 8);

    BitWriter frame;
    for (const auto byte : header.bytes()) frame.write(byte, 8);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        frame.write(0, 1);  // subframe header padding bit
        frame.write(1, 6);  // VERBATIM
        frame.write(0, 1);  // no wasted bits
        for (std::uint32_t f = 0; f < frames; ++f)
            frame.write(static_cast<std::uint16_t>(interleaved[f * channels + channel]), 16);
    }
    frame.pad_to_byte();
    const auto footer = flac_crc16(frame.bytes());
    frame.write(footer, 16);

    out.insert(out.end(), frame.bytes().begin(), frame.bytes().end());
    return out;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Canonical PCM — conversion constants
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the most negative integer code maps to exactly -1.0 at every depth",
          "[authoring-capsule][canonical-pcm]") {
    // The scale is 2^(bits-1), never 2^(bits-1) - 1. An off-by-one full-scale
    // constant would move every sample of every audio member and therefore
    // change every audio member's identity, while still producing audio that
    // sounds correct — so nothing but an exact comparison catches it. Each
    // expectation below is written as a literal rather than derived from the
    // module's own constant, so the test cannot agree with a changed one.
    SECTION("PCM16") {
        const auto wav = make_wav(0x0001, 16, 1, 48000,
                                  pcm16_data({-32768, 32767, -16384, 16384, 1}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->samples[0] == -1.0f);
        // Positive full scale is the standard two's-complement asymmetry:
        // 32767/32768, exact in binary32.
        CHECK(decoded->samples[1] == 0.999969482421875f);
        CHECK(decoded->samples[2] == -0.5f);
        CHECK(decoded->samples[3] == 0.5f);
        // The divisor is a power of two, so even one LSB is exact.
        CHECK(decoded->samples[4] == 0.000030517578125f);
    }

    SECTION("PCM24") {
        const auto wav = make_wav(0x0001, 24, 1, 48000,
                                  pcm24_data({-8388608, 8388607, 1}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->samples[0] == -1.0f);
        CHECK(decoded->samples[1] == 0.99999988079071044921875f);
        CHECK(decoded->samples[2] == 0.00000011920928955078125f);
    }

    SECTION("PCM32") {
        const auto wav =
            make_wav(0x0001, 32, 1, 48000,
                     pcm32_data({std::numeric_limits<std::int32_t>::min(),
                                 std::numeric_limits<std::int32_t>::max(), -1073741824}));
        const auto decoded = decode_to_canonical(wav);
        REQUIRE(decoded.has_value());
        CHECK(decoded->samples[0] == -1.0f);
        // 2147483647/2^31 is below binary32's resolution from 1.0, so the
        // largest positive code rounds to exactly 1.0 — a property of the
        // canonical float format, not of the scaling.
        CHECK(decoded->samples[1] == 1.0f);
        CHECK(decoded->samples[2] == -0.5f);
    }

    SECTION("big-endian AIFF shares the constants") {
        // The same scale reached through a different container: a per-path
        // constant would show up here even though the WAV sections pass.
        const auto aiff = build_iff_form(
            "FORM", "AIFF",
            {{"COMM", aiff_comm_payload(1, 3, 16, extended_rate_be(48000))},
             {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({-32768, 32767, 16384}))}},
            /*big_endian=*/true);
        const auto decoded = decode_to_canonical(aiff);
        REQUIRE(decoded.has_value());
        CHECK(decoded->sample_rate == 48000);
        CHECK(decoded->frame_count == 3);
        CHECK(decoded->samples == std::vector<float>{-1.0f, 0.999969482421875f, 0.5f});
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Canonical PCM — refusals, each named by the rule that caught it
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("a WAV the decoder cannot admit is refused by name",
          "[authoring-capsule][canonical-pcm]") {
    const auto stereo16 = pcm16_data({0, 1, 2, 3});
    const auto good_fmt = wav_fmt_payload(0x0001, 2, 44100, 4, 16);

    // Control first: the same builder produces a container that decodes, so
    // every refusal below is about the one field it changed and not about a
    // fixture the decoder never liked.
    const auto control = build_iff_form("RIFF", "WAVE",
                                        {{"fmt ", good_fmt}, {"data", stereo16}}, false);
    const auto decoded = decode_to_canonical(control);
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels == 2);
    CHECK(decoded->frame_count == 2);

    SECTION("a missing chunk is named rather than defaulted") {
        expect_decode_refused(build_iff_form("RIFF", "WAVE", {{"data", stereo16}}, false),
                              "wav.fmt", "absent");
        expect_decode_refused(build_iff_form("RIFF", "WAVE", {{"fmt ", good_fmt}}, false),
                              "wav.data", "absent");
    }

    SECTION("a truncated fmt chunk cannot be read past its end") {
        auto short_fmt = good_fmt;
        short_fmt.resize(14);
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE", {{"fmt ", short_fmt}, {"data", stereo16}}, false),
            "wav.fmt", "14_bytes");
    }

    SECTION("a chunk that runs past the file is a truncation, not a tail to guess at") {
        // The data chunk declares far more than the container holds. Guessing
        // at the missing bytes would hash audio that was never there.
        expect_decode_refused(build_iff_form("RIFF", "WAVE",
                                             {{"fmt ", good_fmt},
                                              {"data", stereo16, /*declared_size=*/0x00FFFFFF}},
                                             false),
                              "wav.chunk");
        // A form whose own size field cannot even hold the header.
        expect_decode_refused(build_iff_form("RIFF", "WAVE",
                                             {{"fmt ", good_fmt}, {"data", stereo16}}, false,
                                             /*form_size_override=*/0),
                              "wav.size");
    }

    SECTION("a codec that is not linear PCM is refused, never approximated") {
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0002, 1, 44100, 1, 4)}, {"data", stereo16}},
                           false),
            "wav.fmt.audio_format", "wave_format_adpcm(0x0002)");
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0031, 1, 44100, 1, 8)}, {"data", stereo16}},
                           false),
            "wav.fmt.audio_format", "wave_format_gsm610(0x0031)");
        // An entirely unknown tag still names what was found in hex.
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x4242, 1, 44100, 2, 16)}, {"data", stereo16}},
                           false),
            "wav.fmt.audio_format", "0x4242");
    }

    SECTION("a bit depth the canonical form cannot carry exactly") {
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0001, 1, 44100, 1, 8)}, {"data", stereo16}},
                           false),
            "wav.fmt.bits_per_sample", "8");
        // IEEE float is admitted at 32 bits only: float64 would have to be
        // narrowed, and the narrowing is audio the author never rendered.
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0003, 1, 44100, 8, 64)}, {"data", stereo16}},
                           false),
            "wav.fmt.bits_per_sample", "64");
    }

    SECTION("a channel count or rate outside the canonical range") {
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0001, 3, 44100, 6, 16)},
                            {"data", pcm16_data({0, 1, 2, 3, 4, 5})}},
                           false),
            "wav.fmt.channels", "3");
        // No resampling by design: the declared source rate travels inside the
        // hashed header, so an inadmissible rate is a refusal, not a convert.
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0001, 1, 4000, 2, 16)}, {"data", stereo16}},
                           false),
            "wav.fmt.sample_rate", "4000");
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0001, 1, 384000, 2, 16)}, {"data", stereo16}},
                           false),
            "wav.fmt.sample_rate", "384000");
    }

    SECTION("a block alignment that disagrees with the declared frame size") {
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE",
                           {{"fmt ", wav_fmt_payload(0x0001, 2, 44100, 8, 16)}, {"data", stereo16}},
                           false),
            "wav.fmt.block_align", "8");
        // Zero is the one disagreement that is tolerated: writers that leave
        // the field unset are common and the frame size is already known.
        CHECK(decode_to_canonical(
                  build_iff_form("RIFF", "WAVE",
                                 {{"fmt ", wav_fmt_payload(0x0001, 2, 44100, 0, 16)},
                                  {"data", stereo16}},
                                 false))
                  .has_value());
    }

    SECTION("a data chunk that is not a whole number of frames") {
        // Dropping the stray byte would change the bytes the manifest hashes.
        auto partial = stereo16;
        partial.resize(partial.size() - 1);
        expect_decode_refused(
            build_iff_form("RIFF", "WAVE", {{"fmt ", good_fmt}, {"data", partial}}, false),
            "wav.data", "7_bytes");
    }
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE is unwrapped, not trusted",
          "[authoring-capsule][canonical-pcm]") {
    const auto data = pcm16_data({0, 16384, -32768, -16384});

    // Control: a well-formed extensible PCM16 container decodes to exactly the
    // samples a plain PCM16 container of the same content would.
    const auto extensible = build_iff_form(
        "RIFF", "WAVE",
        {{"fmt ", wav_extensible_fmt_payload(0x0001, 1, 48000, 16, 16, ksdataformat_guid(0x0001))},
         {"data", data}},
        false);
    const auto decoded = decode_to_canonical(extensible);
    REQUIRE(decoded.has_value());
    CHECK(decoded->samples == std::vector<float>{0.0f, 0.5f, -1.0f, -0.5f});

    // A container that only claims to be extensible without carrying the
    // extension has no subformat to read.
    expect_decode_refused(
        build_iff_form("RIFF", "WAVE",
                       {{"fmt ", wav_fmt_payload(0xFFFE, 1, 48000, 2, 16)}, {"data", data}}, false),
        "wav.fmt.sub_format", "16_bytes");

    // A GUID whose shared tail does not match is not a KSDATAFORMAT subtype at
    // all, so its first two bytes must not be read as a format tag.
    auto foreign = ksdataformat_guid(0x0001);
    foreign[8] = 0x00;
    expect_decode_refused(
        build_iff_form(
            "RIFF", "WAVE",
            {{"fmt ", wav_extensible_fmt_payload(0x0001, 1, 48000, 16, 16, foreign)},
             {"data", data}},
            false),
        "wav.fmt.sub_format");

    // Valid bits below the container are left-justified and decode exactly;
    // valid bits above it are malformed.
    CHECK(decode_to_canonical(
              build_iff_form("RIFF", "WAVE",
                             {{"fmt ", wav_extensible_fmt_payload(0x0001, 1, 48000, 24, 20,
                                                                  ksdataformat_guid(0x0001))},
                              {"data", pcm24_data({0, 4194304, -8388608})}},
                             false))
              .has_value());
    expect_decode_refused(
        build_iff_form("RIFF", "WAVE",
                       {{"fmt ", wav_extensible_fmt_payload(0x0001, 1, 48000, 16, 24,
                                                            ksdataformat_guid(0x0001))},
                        {"data", data}},
                       false),
        "wav.fmt.valid_bits_per_sample", "24");

    // A subformat that is a real codec is refused by the same rule a plain
    // fmt chunk applies, so wrapping a codec in an extensible header buys
    // nothing.
    expect_decode_refused(
        build_iff_form("RIFF", "WAVE",
                       {{"fmt ", wav_extensible_fmt_payload(0x0007, 1, 48000, 16, 16,
                                                            ksdataformat_guid(0x0007))},
                        {"data", data}},
                       false),
        "wav.fmt.audio_format", "wave_format_mulaw(0x0007)");
}

TEST_CASE("an AIFF the decoder cannot admit is refused by name",
          "[authoring-capsule][canonical-pcm]") {
    const auto rate = extended_rate_be(48000);
    const auto samples = pcm16_be_data({-32768, 0, 16384, 32767});
    const auto good_comm = aiff_comm_payload(2, 2, 16, rate);
    const auto good_ssnd = aiff_ssnd_payload(0, 0, samples);

    // Control: the fixture builder produces an AIFF that decodes.
    const auto control =
        build_iff_form("FORM", "AIFF", {{"COMM", good_comm}, {"SSND", good_ssnd}}, true);
    REQUIRE(decode_to_canonical(control).has_value());

    SECTION("a FORM that is not AIFF or AIFF-C") {
        expect_decode_refused(build_iff_form("FORM", "AIFZ", {}, true), "aiff.form", "AIFZ");
    }

    SECTION("a missing chunk is named rather than defaulted") {
        expect_decode_refused(build_iff_form("FORM", "AIFF", {{"SSND", good_ssnd}}, true),
                              "aiff.comm", "absent");
        expect_decode_refused(build_iff_form("FORM", "AIFF", {{"COMM", good_comm}}, true),
                              "aiff.ssnd", "absent");
    }

    SECTION("a COMM too short for the fields it must carry") {
        auto short_comm = good_comm;
        short_comm.resize(16);
        expect_decode_refused(
            build_iff_form("FORM", "AIFF", {{"COMM", short_comm}, {"SSND", good_ssnd}}, true),
            "aiff.comm", "16_bytes");
        // AIFF-C additionally needs the compression fourcc, so a plain
        // 18-byte COMM is short there even though it is complete for AIFF.
        expect_decode_refused(
            build_iff_form("FORM", "AIFC", {{"COMM", good_comm}, {"SSND", good_ssnd}}, true),
            "aiff.comm", "18_bytes");
    }

    SECTION("an AIFF-C compression type that is a codec") {
        expect_decode_refused(
            build_iff_form("FORM", "AIFC",
                           {{"COMM", aiff_comm_payload(2, 2, 16, rate, "ima4")},
                            {"SSND", good_ssnd}},
                           true),
            "aiff.comm.compression", "ima4");
        expect_decode_refused(
            build_iff_form("FORM", "AIFC",
                           {{"COMM", aiff_comm_payload(2, 2, 16, rate, "ulaw")},
                            {"SSND", good_ssnd}},
                           true),
            "aiff.comm.compression", "ulaw");
    }

    SECTION("the uncompressed AIFF-C spellings decode, each with its own byte order") {
        for (const char* spelling : {"NONE", "twos"}) {
            const auto aifc = build_iff_form(
                "FORM", "AIFC",
                {{"COMM", aiff_comm_payload(1, 2, 16, rate, spelling)},
                 {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({-32768, 16384}))}},
                true);
            const auto big = decode_to_canonical(aifc);
            REQUIRE(big.has_value());
            CHECK(big->samples == std::vector<float>{-1.0f, 0.5f});
        }

        // `sowt` is the same PCM written little-endian. The identical byte
        // sequence must therefore decode to different samples than `twos`,
        // which is what proves the byte order is honoured rather than ignored.
        const auto raw = pcm16_be_data({-32768, 16384});
        const auto swapped = build_iff_form(
            "FORM", "AIFC",
            {{"COMM", aiff_comm_payload(1, 2, 16, rate, "sowt")}, {"SSND", aiff_ssnd_payload(0, 0, raw)}},
            true);
        const auto little = decode_to_canonical(swapped);
        REQUIRE(little.has_value());
        // 0x8000 and 0x4000 read the other way round are 0x0080 and 0x0040.
        CHECK(little->samples == std::vector<float>{0.00390625f, 0.001953125f});

        const auto floats = build_iff_form(
            "FORM", "AIFC",
            {{"COMM", aiff_comm_payload(1, 2, 32, rate, "fl32")},
             {"SSND", aiff_ssnd_payload(0, 0, float32_be_data({0.25f, -0.75f}))}},
            true);
        const auto decoded_floats = decode_to_canonical(floats);
        REQUIRE(decoded_floats.has_value());
        CHECK(decoded_floats->samples == std::vector<float>{0.25f, -0.75f});
    }

    SECTION("a sample size the spelling does not define") {
        // `sowt` is defined for 16-bit sample points only.
        expect_decode_refused(
            build_iff_form("FORM", "AIFC",
                           {{"COMM", aiff_comm_payload(1, 2, 24, rate, "sowt")},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm24_data({0, 1}))}},
                           true),
            "aiff.comm.sample_size", "24");
        // `fl32` is 32-bit float by definition.
        expect_decode_refused(
            build_iff_form("FORM", "AIFC",
                           {{"COMM", aiff_comm_payload(1, 2, 16, rate, "fl32")},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_size", "16");
        // Below 9 bits there is no whole-byte container to decode from.
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 8, rate)},
                            {"SSND", aiff_ssnd_payload(0, 0, {0, 1})}},
                           true),
            "aiff.comm.sample_size", "8");
    }

    SECTION("a channel count or rate outside the canonical range") {
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(3, 1, 16, rate)},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1, 2}))}},
                           true),
            "aiff.comm.channels", "3");
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 16, extended_rate_be(4000))},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_rate", "4000");
        // An all-zero extended field is a zero rate, not a missing one.
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 16, std::vector<std::uint8_t>(10, 0))},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_rate", "0");
    }

    SECTION("an extended rate that is not an exact positive integer") {
        // 44100.5 Hz: mantissa carries a fractional bit. Rounding it would put
        // a rate in the hashed header that the source never declared.
        auto fractional = extended_rate_be(88201);
        const auto biased = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(fractional[0]) << 8) | fractional[1]);
        fractional[0] = static_cast<std::uint8_t>(((biased - 1) >> 8) & 0xFF);
        fractional[1] = static_cast<std::uint8_t>((biased - 1) & 0xFF);
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 16, fractional)},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_rate", "fractional_hz");

        auto negative = extended_rate_be(48000);
        negative[0] = static_cast<std::uint8_t>(negative[0] | 0x80);
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 16, negative)},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_rate", "negative");

        std::vector<std::uint8_t> not_finite(10, 0);
        not_finite[0] = 0x7F;
        not_finite[1] = 0xFF;
        not_finite[2] = 0x80;
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 16, not_finite)},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_rate", "infinity_or_nan");

        // A rate whose magnitude does not fit 64 bits at all.
        std::vector<std::uint8_t> enormous(10, 0);
        enormous[0] = 0x7F;
        enormous[1] = 0xFE;
        enormous[2] = 0x80;
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(1, 2, 16, enormous)},
                            {"SSND", aiff_ssnd_payload(0, 0, pcm16_be_data({0, 1}))}},
                           true),
            "aiff.comm.sample_rate", "wider_than_64_bits");
    }

    SECTION("sound data that does not match what COMM declared") {
        // Short of the prolog entirely.
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", good_comm}, {"SSND", std::vector<std::uint8_t>{0, 0, 0, 0}}},
                           true),
            "aiff.ssnd", "4_bytes");
        // Block-aligned sound data would have to be deblocked, which is a
        // rewrite of the audio.
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", good_comm}, {"SSND", aiff_ssnd_payload(0, 512, samples)}},
                           true),
            "aiff.ssnd.block_size", "512");
        // Slack bytes are audio nobody declared; a short chunk is a truncation.
        auto extra = samples;
        extra.push_back(0);
        extra.push_back(0);
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", good_comm}, {"SSND", aiff_ssnd_payload(0, 0, extra)}},
                           true),
            "aiff.ssnd.bytes", "10_bytes");
        expect_decode_refused(
            build_iff_form("FORM", "AIFF",
                           {{"COMM", aiff_comm_payload(2, 4, 16, rate)},
                            {"SSND", aiff_ssnd_payload(0, 0, samples)}},
                           true),
            "aiff.ssnd.bytes", "8_bytes");
    }
}

TEST_CASE("a container this build does not decode is named, never approximated",
          "[authoring-capsule][canonical-pcm]") {
    // 64-bit RIFF variants carry their real sizes outside the 32-bit fields
    // this reader understands, so admitting them would decode the wrong span.
    expect_decode_refused(bytes_of("RF64----WAVEmore"), "audio.container", "rf64");
    expect_decode_refused(bytes_of("BW64----WAVEmore"), "audio.container", "bw64");
    // An ISO base media file: MP4/M4A, whose codecs are all inadmissible. The
    // magic sits at offset 4, so a reader that only looked at the first four
    // bytes would fall through to the unrecognized-fourcc branch.
    expect_decode_refused(
        std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '},
        "audio.container", "iso-bmff");
    // A RIFF that is not WAVE at all: the form type is what decides, not the
    // RIFF magic.
    expect_decode_refused(build_iff_form("RIFF", "AVI ", {}, false), "audio.container",
                          "riff:AVI ");
    // Too short to hold any magic: the refusal says so rather than reading
    // past the end.
    expect_decode_refused(bytes_of("ab"), "audio.container", "truncated:2_bytes");
    // A leading fourcc that is not printable is reported as hex, so the
    // message stays unambiguous for a corrupt file.
    expect_decode_refused(std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03, 0x04, 0x05},
                          "audio.container", "00010203");
}

TEST_CASE("a canonical member is decoded from its own header, or refused",
          "[authoring-capsule][canonical-pcm]") {
    // Control: the fixture builder agrees with the module's own serializer,
    // so every refusal below is about the one field it changed.
    const auto built = build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion, 1, 48000, 2,
                                              {0.25f, -0.5f});
    REQUIRE(built == to_canonical_bytes(make_pcm(1, 48000, {0.25f, -0.5f})));
    REQUIRE(from_canonical_bytes(built).has_value());

    const auto refused = [](const std::vector<std::uint8_t>& bytes, std::string_view subject,
                            std::string_view found) {
        INFO(subject);
        const auto parsed = from_canonical_bytes(bytes);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().status == CapsuleStatus::decode_unsupported);
        CHECK(parsed.error().subject == subject);
        CHECK(parsed.error().found == found);
    };

    // Shorter than the fixed header: there is no geometry to read.
    auto truncated = built;
    truncated.resize(kCanonicalPcmHeaderBytes - 1);
    refused(truncated, "canonical_pcm", "27_bytes");

    refused(build_canonical_member("pulp.wav", kCanonicalPcmDecoderVersion, 1, 48000, 2,
                                   {0.25f, -0.5f}),
            "canonical_pcm.magic", "70756c702e776176");

    // The version guard binds to the bytes, so bytes minted by a decoder this
    // build does not implement can never be re-read as this version's
    // rendition — whatever metadata travelled beside them.
    refused(build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion + 1, 1, 48000, 2,
                                   {0.25f, -0.5f}),
            "canonical_pcm.decoder_version", std::to_string(kCanonicalPcmDecoderVersion + 1));

    refused(build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion, 0, 48000, 0, {}),
            "canonical_pcm.channels", "0");
    refused(build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion, 3, 48000, 1,
                                   {0.0f, 0.0f, 0.0f}),
            "canonical_pcm.channels", "3");

    // A frame count whose byte span is not even representable is refused
    // before the multiplication that would wrap.
    const std::uint64_t unrepresentable = std::numeric_limits<std::uint64_t>::max() / 4 + 1;
    refused(build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion, 1, 48000,
                                   unrepresentable, {}),
            "canonical_pcm.frame_count", std::to_string(unrepresentable));

    // Trailing bytes would be content the header never declared, hashed into
    // the identity but never played.
    auto trailing = built;
    trailing.push_back(0);
    trailing.push_back(0);
    trailing.push_back(0);
    trailing.push_back(0);
    refused(trailing, "canonical_pcm.bytes", "12_bytes");
    refused(build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion, 1, 48000, 4,
                                   {0.25f, -0.5f}),
            "canonical_pcm.bytes", "8_bytes");

    // The rate is validated on the way out of the parse too, so a member whose
    // header claims an inadmissible rate cannot be handed to a caller.
    refused(build_canonical_member("pulp.pcm", kCanonicalPcmDecoderVersion, 1, 1000, 2,
                                   {0.25f, -0.5f}),
            "canonical_pcm.sample_rate", "1000");
}

TEST_CASE("validate_canonical refuses a geometry that disagrees with the buffer",
          "[authoring-capsule][canonical-pcm]") {
    // Control: the same struct with a consistent geometry validates.
    REQUIRE(validate_canonical(make_pcm(2, 48000, {0.0f, 0.0f, 0.0f, 0.0f})).has_value());

    CanonicalPcm short_buffer = make_pcm(2, 48000, {0.0f, 0.0f, 0.0f, 0.0f});
    short_buffer.frame_count = 3;
    const auto mismatched = validate_canonical(short_buffer);
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().status == CapsuleStatus::decode_unsupported);
    CHECK(mismatched.error().subject == "canonical_pcm.frame_count");
    // The buffer's own frame count is reported, so the caller can see which
    // side is wrong rather than only that the two disagree.
    CHECK(mismatched.error().required == "2_frames");
    CHECK(mismatched.error().found == "3_frames");

    const auto bad_rate = validate_canonical(make_pcm(1, 7999, {0.0f}));
    REQUIRE_FALSE(bad_rate.has_value());
    CHECK(bad_rate.error().subject == "canonical_pcm.sample_rate");
    CHECK(bad_rate.error().found == "7999");

    // Control: mono at one frame is admissible, so the channel refusal below
    // is about the count and not about the one-sample buffer.
    REQUIRE(validate_canonical(make_pcm(1, 48000, {0.0f})).has_value());
    CanonicalPcm too_many = make_pcm(1, 48000, {0.0f});
    too_many.channels = 5;
    too_many.frame_count = 0;
    const auto refused_channels = validate_canonical(too_many);
    REQUIRE_FALSE(refused_channels.has_value());
    CHECK(refused_channels.error().subject == "canonical_pcm.channels");
    CHECK(refused_channels.error().found == "5");
}

// ═══════════════════════════════════════════════════════════════════════════
// Private staging, extraction, and no-replace publication
// ═══════════════════════════════════════════════════════════════════════════

#if !defined(_WIN32)
// Owner-only permissions are a POSIX mode question here. The Windows staging
// path expresses the same property through a protected DACL and is asserted on
// that platform, not emulated on this one: a stand-in check would report a
// property nothing on this host actually enforces.
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

void write_scratch_file(const fs::path& path, std::string_view contents) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    REQUIRE(fs::exists(path));
}

/// The fixture capsule, opened and previewed. `preview.manifest` is the
/// measured closure, so a staging test starts from rows whose digests and
/// sizes describe the bytes that actually travelled.
struct OpenedFixture {
    CapsuleArchive archive;
    CapsulePreview preview;
};

OpenedFixture open_fixture_capsule(const fs::path& destination) {
    auto exported = export_capsule(make_export_request(), destination);
    REQUIRE(exported.has_value());
    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());
    AdmissionOptions options;
    options.product = "test-product";
    auto preview = preview_capsule(*archive, test_registry(), options);
    REQUIRE(preview.has_value());
    // Both fixture members are declared, and in canonical path order.
    REQUIRE(preview->manifest.files.size() == 2);
    REQUIRE(preview->manifest.files[0].path == "audio/render.pcm");
    REQUIRE(preview->manifest.files[1].path == "dsp/main.cpp");
    return OpenedFixture{std::move(archive).value(), std::move(preview).value()};
}

}  // namespace

TEST_CASE("StagingArea::create refuses a parent it cannot stage under",
          "[authoring-capsule][staging]") {
    TempDir temp;

    // Control: a real directory yields a staging area, so every refusal below
    // is about the parent it was handed.
    {
        auto usable = StagingArea::create(temp.path());
        REQUIRE(usable.has_value());
        CHECK(fs::is_directory(usable->root()));
        // The staged tree lives under the parent the caller chose, never
        // beside it.
        CHECK(usable->root().parent_path() == fs::canonical(temp.path()));
    }

    const auto refused = [](const fs::path& parent) {
        INFO(parent.string());
        auto result = StagingArea::create(parent);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().status == CapsuleStatus::staging_failed);
    };

    // A parent that does not exist is not created: staging places a tree under
    // a directory the consumer already chose.
    refused(temp.path() / "absent");
    CHECK_FALSE(fs::exists(temp.path() / "absent"));

    // A regular file resolves as a path but is not somewhere a tree can go.
    const fs::path file = temp.path() / "not-a-directory";
    write_scratch_file(file, "x");
    refused(file);
    // The file is left exactly as it was found.
    CHECK(fs::is_regular_file(file));
    CHECK(fs::file_size(file) == 1);

    refused(fs::path{});
}

#if !defined(_WIN32)
TEST_CASE("a staging area and its members are readable only by their owner",
          "[authoring-capsule][staging]") {
    TempDir temp;
    const fs::path destination = temp.path() / "fixture.capsule";
    auto fixture = open_fixture_capsule(destination);

    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());

    struct stat directory {};
    REQUIRE(::stat(staging->root().c_str(), &directory) == 0);
    CHECK(S_ISDIR(directory.st_mode));
    CHECK(directory.st_uid == ::geteuid());
    // Exactly 0700. Another account must not be able to read a capsule's
    // contents mid-import, nor swap a member under the validator, so group and
    // other carry no bit at all — not even execute, which would let a sibling
    // account traverse into the tree.
    CHECK((directory.st_mode & 07777) == 0700);

    REQUIRE(extract_declared(fixture.archive, fixture.preview.manifest, *staging).has_value());

    for (const auto& entry : fixture.preview.manifest.files) {
        INFO(entry.path);
        struct stat member {};
        REQUIRE(::stat((staging->root() / entry.path).c_str(), &member) == 0);
        CHECK(S_ISREG(member.st_mode));
        CHECK((member.st_mode & (S_IRWXG | S_IRWXO)) == 0);
        // The intermediate directory the member needed is private too; a
        // world-traversable parent would expose the member whatever its own
        // mode says.
        struct stat parent {};
        REQUIRE(::stat((staging->root() / fs::path(entry.path).parent_path()).c_str(), &parent) ==
                0);
        CHECK((parent.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    }
}
#endif

TEST_CASE("an unpublished staging area removes its tree when it goes out of scope",
          "[authoring-capsule][staging]") {
    TempDir temp;
    fs::path root;
    {
        auto staging = StagingArea::create(temp.path());
        REQUIRE(staging.has_value());
        root = staging->root();
        REQUIRE(fs::is_directory(root));
        // A populated tree, so the test proves a recursive removal rather than
        // an empty-directory rmdir that would succeed either way.
        fs::create_directories(root / "dsp");
        write_scratch_file(root / "dsp" / "main.cpp", "int main() { return 0; }\n");
    }
    CHECK_FALSE(fs::exists(root));
    // Only the staged tree goes: the parent the consumer chose is left alone.
    CHECK(fs::is_directory(temp.path()));
}

TEST_CASE("publishing hands the tree over, so destruction no longer removes it",
          "[authoring-capsule][staging]") {
    TempDir temp;
    const fs::path destination = temp.path() / "project";
    fs::path root;
    {
        auto staging = StagingArea::create(temp.path());
        REQUIRE(staging.has_value());
        root = staging->root();
        write_scratch_file(root / "member.bin", "payload");

        REQUIRE(staging->publish_no_replace(destination).has_value());
        CHECK(fs::exists(destination / "member.bin"));
        CHECK_FALSE(fs::exists(root));

        // A published area owns nothing more. Publishing again must not move a
        // project that is already live under its final name.
        const auto again = staging->publish_no_replace(temp.path() / "second");
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().status == CapsuleStatus::staging_failed);
        CHECK_FALSE(fs::exists(temp.path() / "second"));
        CHECK(fs::exists(destination / "member.bin"));
    }
    // Destruction after publication leaves the published tree standing.
    CHECK(fs::exists(destination / "member.bin"));
}

TEST_CASE("publish_no_replace refuses a destination it was not given a place for",
          "[authoring-capsule][staging]") {
    TempDir temp;
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    write_scratch_file(staging->root() / "member.bin", "payload");

    const auto refused = [&staging](const fs::path& destination) {
        INFO(destination.string());
        const auto result = staging->publish_no_replace(destination);
        REQUIRE_FALSE(result.has_value());
        // Not `publication_conflict`: nothing occupies the name. The
        // destination was never a place a tree could go.
        CHECK(result.error().status == CapsuleStatus::staging_failed);
    };

    // The destination's parent is not created here. Inventing a directory the
    // consumer never chose is a decision this layer does not own.
    refused(temp.path() / "absent" / "project");
    CHECK_FALSE(fs::exists(temp.path() / "absent"));

    // A trailing separator makes `parent_path()` name the destination itself,
    // so publishing it would place the tree somewhere the caller did not ask
    // for. A destination must name a leaf. The parent here deliberately
    // exists: without the leaf rule the rename would target `holder` itself,
    // which is a different refusal (`publication_conflict`) or, on an empty
    // holder, a publication the caller never asked for.
    const fs::path holder = temp.path() / "holder";
    fs::create_directory(holder);
    refused(holder / "");
    CHECK(fs::is_empty(holder));

    refused(fs::path{});

    // Control: the same staging area publishes to a well-formed destination,
    // which is what proves the refusals were about the destinations and not
    // about an area that could never publish at all.
    const fs::path good = temp.path() / "project";
    REQUIRE(staging->publish_no_replace(good).has_value());
    CHECK(fs::exists(good / "member.bin"));
}

TEST_CASE("extract_declared writes exactly the declared members",
          "[authoring-capsule][staging]") {
    TempDir temp;
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());

    std::vector<std::size_t> observed;
    std::size_t reported_count = 0;
    const ExtractionProgress progress = [&](std::size_t index, std::size_t count) {
        observed.push_back(index);
        reported_count = count;
        return true;
    };

    REQUIRE(extract_declared(fixture.archive, fixture.preview.manifest, *staging, progress)
                .has_value());

    // Progress is reported once per declared member, in manifest order.
    CHECK(observed == std::vector<std::size_t>{0, 1});
    CHECK(reported_count == fixture.preview.manifest.files.size());

    for (const auto& entry : fixture.preview.manifest.files) {
        INFO(entry.path);
        const auto staged = staging->root() / entry.path;
        REQUIRE(fs::is_regular_file(staged));
        const auto bytes = read_file(staged);
        // Byte-for-byte what the archive holds, not merely the right length.
        const auto expanded = fixture.archive.read(entry.path);
        REQUIRE(expanded.has_value());
        CHECK(bytes == *expanded);
        CHECK(pulp::runtime::sha256_hex(bytes.data(), bytes.size()) == entry.sha256);
    }

    // The manifest member itself is not a declared row, so it is not written:
    // extraction iterates the manifest, never the archive.
    CHECK_FALSE(fs::exists(staging->root() / std::string(kManifestPath)));
}

TEST_CASE("cancelling extraction leaves nothing published and the destination untouched",
          "[authoring-capsule][staging]") {
    TempDir temp;
    const fs::path destination = temp.path() / "project";
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");

    fs::path root;
    {
        auto staging = StagingArea::create(temp.path());
        REQUIRE(staging.has_value());
        root = staging->root();

        // Cancel on the second member, so the first has demonstrably landed:
        // a run that cancelled before doing any work would pass a test that
        // only checked the second member's absence.
        std::vector<std::size_t> observed;
        const ExtractionProgress progress = [&](std::size_t index, std::size_t) {
            observed.push_back(index);
            return index < 1;
        };

        const auto cancelled =
            extract_declared(fixture.archive, fixture.preview.manifest, *staging, progress);
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().status == CapsuleStatus::cancelled);
        // Named by the member the import stopped on.
        CHECK(cancelled.error().subject == "dsp/main.cpp");
        CHECK(observed == std::vector<std::size_t>{0, 1});

        // Cancellation is checked before the member is read, so the member it
        // stopped on was neither expanded nor written.
        CHECK(fs::exists(root / "audio" / "render.pcm"));
        CHECK_FALSE(fs::exists(root / "dsp"));
        CHECK_FALSE(fs::exists(root / "dsp" / "main.cpp"));

        // Nothing outside the private tree was touched.
        CHECK_FALSE(fs::exists(destination));
    }

    // And the partial tree does not survive the staging area that owned it.
    CHECK_FALSE(fs::exists(root));
    CHECK_FALSE(fs::exists(destination));
    // Control: the parent the consumer chose still holds only what it started
    // with, so the absence above is not an absent parent.
    CHECK(fs::is_directory(temp.path()));
}

TEST_CASE("extract_declared refuses a row the archive does not carry",
          "[authoring-capsule][staging]") {
    TempDir temp;
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());

    // A declared-but-absent row is a closure violation, named by the path the
    // manifest asserted, rather than surfacing as an opaque read failure. The
    // membership scan and the archive reader agree on that status, so this
    // pins the contract rather than which of the two spoke first. The row is
    // placed first so the refusal cannot be confused with a partial extract.
    Manifest manifest = fixture.preview.manifest;
    FileEntry absent = manifest.files.front();
    absent.path = "audio/absent.pcm";
    manifest.files.insert(manifest.files.begin(), absent);

    const auto refused = extract_declared(fixture.archive, manifest, *staging);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().status == CapsuleStatus::closure_violation);
    CHECK(refused.error().subject == "audio/absent.pcm");
    CHECK(fs::is_empty(staging->root()));

    // Control: the same archive and staging area extract the unmodified
    // manifest, so the refusal is about the added row.
    REQUIRE(extract_declared(fixture.archive, fixture.preview.manifest, *staging).has_value());
    CHECK(fs::exists(staging->root() / "audio" / "render.pcm"));
}

TEST_CASE("extract_declared applies the full path grammar, not just containment",
          "[authoring-capsule][staging]") {
    TempDir temp;
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");

    // `extract_declared` is a public entry point taking an arbitrary Manifest,
    // so it cannot assume a caller ran preview first. A path that never faced
    // admission would otherwise reach the filesystem through a caller that
    // skipped it.
    const auto refused = [&fixture, &temp](std::string path, std::string_view rule) {
        INFO(path);
        auto staging = StagingArea::create(temp.path());
        REQUIRE(staging.has_value());

        Manifest manifest = fixture.preview.manifest;
        FileEntry hostile = manifest.files.front();
        hostile.path = std::move(path);
        manifest.files.insert(manifest.files.begin(), hostile);

        const auto result = extract_declared(fixture.archive, manifest, *staging);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().status == CapsuleStatus::path_rejected);
        // The rule token is what proves the full grammar ran: a containment-only
        // check would have to refuse for a different reason, or not at all.
        CHECK(result.error().required == rule);
        CHECK(fs::is_empty(staging->root()));
        // Nothing landed beside the staging area either.
        CHECK_FALSE(fs::exists(temp.path() / "escape.txt"));
    };

    // A reserved device name is contained, relative, and free of dot-dot: a
    // containment check would pass it, and on Windows it would open a device.
    refused("CON", "no-reserved-device-name");
    refused("dsp/PRN", "no-reserved-device-name");
    // Traversal, which both layers refuse.
    refused("../escape.txt", "no-dot-component");
    // A leading slash makes the path absolute on extraction.
    refused("/etc/passwd", "relative-path");
    // A trailing space or dot is silently stripped by some filesystems, so two
    // declared rows could collapse onto one file.
    refused("dsp/main.cpp ", "no-trailing-dot-or-space");
}

TEST_CASE("read_staged_member joins the path inside the layer that owns admission",
          "[authoring-capsule][staging]") {
    TempDir temp;
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    REQUIRE(extract_declared(fixture.archive, fixture.preview.manifest, *staging).has_value());

    const FileEntry& audio = fixture.preview.manifest.files[0];
    const FileEntry& source = fixture.preview.manifest.files[1];

    // Control: both members read back exactly the bytes the archive holds.
    for (const FileEntry* entry : {&audio, &source}) {
        INFO(entry->path);
        const auto read_back = read_staged_member(*staging, *entry);
        REQUIRE(read_back.has_value());
        const auto expanded = fixture.archive.read(entry->path);
        REQUIRE(expanded.has_value());
        CHECK(*read_back == *expanded);
        CHECK(pulp::runtime::sha256_hex(read_back->data(), read_back->size()) == entry->sha256);
    }

    // The canonical member reads back as decodable audio, which is the whole
    // point of reading one: the bytes are usable without a second admission.
    const auto audio_bytes = read_staged_member(*staging, audio);
    REQUIRE(audio_bytes.has_value());
    const auto decoded = from_canonical_bytes(*audio_bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded->sample_rate == 48000);

    SECTION("a size the tree does not hold is refused before the bytes are copied") {
        FileEntry oversized = audio;
        oversized.bytes = audio.bytes + 1;
        const auto refused = read_staged_member(*staging, oversized);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::staging_failed);
        CHECK(refused.error().subject == audio.path);
        // Both sides are reported, so a caller can see which one is wrong.
        CHECK(refused.error().required == std::to_string(audio.bytes + 1));
        CHECK(refused.error().found == std::to_string(audio.bytes));
    }

    SECTION("a path the grammar refuses never reaches the filesystem") {
        FileEntry escaping = audio;
        escaping.path = "../render.pcm";
        const auto refused = read_staged_member(*staging, escaping);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::path_rejected);
        CHECK(refused.error().required == "no-dot-component");

        FileEntry device = audio;
        device.path = "NUL";
        const auto reserved = read_staged_member(*staging, device);
        REQUIRE_FALSE(reserved.has_value());
        CHECK(reserved.error().status == CapsuleStatus::path_rejected);
        CHECK(reserved.error().required == "no-reserved-device-name");
    }

    SECTION("a member that is not there, or is not a plain file") {
        FileEntry missing = audio;
        missing.path = "audio/absent.pcm";
        const auto absent = read_staged_member(*staging, missing);
        REQUIRE_FALSE(absent.has_value());
        CHECK(absent.error().status == CapsuleStatus::staging_failed);
        CHECK(absent.error().subject == "audio/absent.pcm");

        // A directory standing where a member belongs means the tree is not
        // the one extraction wrote, so it is refused rather than read.
        FileEntry directory = audio;
        directory.path = "audio";
        directory.bytes = 0;
        const auto refused = read_staged_member(*staging, directory);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::staging_failed);
    }
}

TEST_CASE("a staging root reaches the same read as the staging area that owns it",
          "[authoring-capsule][staging]") {
    // `ProfileValidator::validate_staged()` is handed the root as a path, and a
    // `StagingArea` cannot be adopted from an existing tree. A profile that
    // cannot reach this entry point joins the untrusted member path to the root
    // itself, which is the duplication the join exists to prevent.
    TempDir temp;
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");
    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    REQUIRE(extract_declared(fixture.archive, fixture.preview.manifest, *staging).has_value());

    const std::filesystem::path root = staging->root();

    for (const FileEntry& entry : fixture.preview.manifest.files) {
        INFO(entry.path);
        const auto owned = read_staged_member(*staging, entry);
        const auto by_root = read_staged_member(root, entry);
        REQUIRE(owned.has_value());
        REQUIRE(by_root.has_value());
        CHECK(*by_root == *owned);
        CHECK(pulp::runtime::sha256_hex(by_root->data(), by_root->size()) == entry.sha256);
    }

    SECTION("the admission travels with the root, not with the staging area") {
        const FileEntry& audio = fixture.preview.manifest.files[0];

        FileEntry escaping = audio;
        escaping.path = "../render.pcm";
        const auto refused = read_staged_member(root, escaping);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::path_rejected);
        CHECK(refused.error().required == "no-dot-component");

        FileEntry oversized = audio;
        oversized.bytes = audio.bytes + 1;
        const auto mismatched = read_staged_member(root, oversized);
        REQUIRE_FALSE(mismatched.has_value());
        CHECK(mismatched.error().status == CapsuleStatus::staging_failed);
        CHECK(mismatched.error().required == std::to_string(audio.bytes + 1));
        CHECK(mismatched.error().found == std::to_string(audio.bytes));
    }

    SECTION("an empty root is refused rather than resolved against the process directory") {
        const auto refused = read_staged_member(std::filesystem::path{},
                                                fixture.preview.manifest.files[0]);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::staging_failed);
    }
}

TEST_CASE("a FLAC source decodes, and to the same samples as the WAV of that content",
          "[authoring-capsule][canonical-pcm]") {
    // The control the FLAC refusal cases have been missing: without a stream
    // this decoder actually admits, a decode_flac that refused everything
    // would satisfy every negative case above.
    std::vector<std::int16_t> interleaved;
    for (int i = 0; i < 32; ++i)
        interleaved.push_back(static_cast<std::int16_t>(i * 2113 - 32768));

    const auto decoded = decode_to_canonical(make_flac(interleaved, 2, 48000));
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels == 2);
    CHECK(decoded->sample_rate == 48000);
    CHECK(decoded->frame_count == 16);
    REQUIRE(decoded->samples.size() == interleaved.size());

    // The documented cross-format property: FLAC and WAV sources of equal
    // content produce identical samples, so a bank re-encoded from one to the
    // other keeps its identity. Asserted bit-exactly — a tolerance here would
    // pass for a scaling that is merely close, and "close" mints a different
    // SHA-256.
    const auto from_wav =
        decode_to_canonical(make_wav(0x0001, 16, 2, 48000, pcm16_data(interleaved)));
    REQUIRE(from_wav.has_value());
    CHECK(decoded->samples == from_wav->samples);
    CHECK(canonical_pcm_digest(*decoded) == canonical_pcm_digest(*from_wav));

    SECTION("a declared rate outside the canonical range") {
        expect_decode_refused(make_flac(interleaved, 2, 4000), "flac.sample_rate", "4000");
    }

    SECTION("more channels than the canonical representation carries") {
        std::vector<std::int16_t> three_channel;
        for (int i = 0; i < 24; ++i)
            three_channel.push_back(static_cast<std::int16_t>(i * 1000 - 12000));
        expect_decode_refused(make_flac(three_channel, 3, 48000), "flac.channels", "3");
    }

    SECTION("a stream whose length STREAMINFO never declared") {
        // Zero total samples is how a streamed encode spells "unknown". The
        // member budget has to be provable before allocation, so it is a
        // refusal rather than an unbounded read.
        expect_decode_refused(make_flac(interleaved, 2, 48000, 0),
                              "flac.total_pcm_frame_count", "unknown");
    }

    SECTION("a stream that ships fewer frames than it promised") {
        // Padding the difference would hash audio that was never there.
        expect_decode_refused(make_flac(interleaved, 2, 48000, 64), "flac.frames", "16_frames");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Canonical JSON — the refusals
// ═══════════════════════════════════════════════════════════════════════════
//
// Canonicalization runs over bytes from another machine, and every rule it
// enforces exists because the alternative is a digest taken over something
// other than what was parsed. Each refusal below is asserted with the exact
// pointer or `found` token it reports: a serializer that refused everything
// for one reason would satisfy a test that only checked "this failed".

namespace {

/// Canonicalize a one-member object, so a case can assert either the bytes or
/// the rejection without assembling an envelope around it.
pulp::runtime::Result<std::string, CapsuleError> canonical_member(std::string_view name,
                                                                  choc::value::Value value) {
    auto object = choc::value::createObject("");
    object.addMember(name, std::move(value));
    return detail::to_canonical_text(object.getView());
}

std::string quoted_or_fail(std::string_view raw) {
    auto text = canonical_member("s", choc::value::createString(raw));
    REQUIRE(text.has_value());
    return text.value();
}

void expect_string_refused(std::string_view raw, std::string_view found) {
    INFO(found);
    auto text = canonical_member("s", choc::value::createString(raw));
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().status == CapsuleStatus::manifest_invalid);
    CHECK(text.error().required == "well-formed UTF-8");
    CHECK(text.error().found == found);
}

}  // namespace

TEST_CASE("a JSON pointer escapes the characters that would make it ambiguous",
          "[authoring-capsule][canonical-json]") {
    // RFC 6901 spells `~` as `~0` and `/` as `~1`. Without that, a member
    // named `a/b` would report as `/a/b` — the pointer for a member `b` of a
    // nested object `a`, which names a different node entirely.
    const auto refused = canonical_member(
        "a/b~c", choc::value::createFloat64(std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().subject == "/a~1b~0c");

    // Control: the same member with a finite value serializes, and the key
    // itself is carried through unescaped — only the pointer is escaped.
    auto ok = canonical_member("a/b~c", choc::value::createFloat64(1.0));
    REQUIRE(ok.has_value());
    CHECK(ok.value() == "{\"a/b~c\":1}");
}

TEST_CASE("a rejection names the type it found", "[authoring-capsule][canonical-json]") {
    // `parse_json_object` is the gate every envelope-shaped blob goes through,
    // and "expected an object, found an array" is the difference between a
    // person fixing their manifest and guessing at it.
    const auto expect_type = [](std::string_view text, std::string_view type) {
        INFO(text);
        const auto parsed = detail::parse_json_object(text, "/topology");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().status == CapsuleStatus::manifest_invalid);
        CHECK(parsed.error().subject == "/topology");
        CHECK(parsed.error().required == "object");
        CHECK(parsed.error().found == type);
    };
    expect_type("[1,2]", "array");

    // A bare scalar never reaches that check: the prescan refuses it first,
    // because no outermost bracket ever closed. Asserted so the two refusals
    // stay distinguishable — "found an array" is a schema problem the author
    // can fix, "no value" is not the same complaint.
    for (const auto* scalar : {"\"text\"", "5", "1.5", "true"}) {
        INFO(scalar);
        const auto refused = detail::parse_json_object(scalar, "/topology");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().required == "a JSON object or array");
        CHECK(refused.error().found == "no value");
    }

    // Control: an object is admitted, so the refusals above are about the type
    // rather than about a parser that refuses every input.
    CHECK(detail::parse_json_object("{}", "/topology").has_value());
}

TEST_CASE("canonical numbers cover the sign and both exponent layouts",
          "[authoring-capsule][canonical-json]") {
    const auto canonical = [](double value) {
        auto text = detail::canonical_number(value);
        REQUIRE(text.has_value());
        return text.value();
    };

    // The sign travels with the digits; it is not a separate field a layout
    // rule could drop.
    CHECK(canonical(-1.0) == "-1");
    CHECK(canonical(-1.5) == "-1.5");
    CHECK(canonical(-0.25) == "-0.25");
    CHECK(canonical(-100.0) == "-100");
    CHECK(canonical(-1e21) == "-1e+21");
    CHECK(canonical(-1e-7) == "-1e-7");

    // A mantissa of more than one digit takes the `d.ddd` exponent form, which
    // is a different layout branch from the single-digit `de+21` case.
    CHECK(canonical(1.5e21) == "1.5e+21");
    CHECK(canonical(1.25e-7) == "1.25e-7");
    CHECK(canonical(-1.5e21) == "-1.5e+21");
}

TEST_CASE("canonical strings escape exactly what JSON requires",
          "[authoring-capsule][canonical-json]") {
    // The two structural characters, then the five named escapes. Emitting a
    // raw newline inside a string would produce text no JSON parser accepts,
    // and the digest would then cover bytes nothing can read back.
    CHECK(quoted_or_fail("a\"b") == "{\"s\":\"a\\\"b\"}");
    CHECK(quoted_or_fail("a\\b") == "{\"s\":\"a\\\\b\"}");
    CHECK(quoted_or_fail("a\bb") == "{\"s\":\"a\\bb\"}");
    CHECK(quoted_or_fail("a\fb") == "{\"s\":\"a\\fb\"}");
    CHECK(quoted_or_fail("a\nb") == "{\"s\":\"a\\nb\"}");
    CHECK(quoted_or_fail("a\rb") == "{\"s\":\"a\\rb\"}");
    CHECK(quoted_or_fail("a\tb") == "{\"s\":\"a\\tb\"}");

    // Every other C0 control takes the six-character form in lowercase hex, so
    // two writers cannot disagree on the spelling of the same byte.
    CHECK(quoted_or_fail(std::string_view("a\x01z", 3)) == "{\"s\":\"a\\u0001z\"}");
    CHECK(quoted_or_fail(std::string_view("\x1f", 1)) == "{\"s\":\"\\u001f\"}");
}

TEST_CASE("canonical strings admit every well-formed UTF-8 sequence and refuse the rest",
          "[authoring-capsule][canonical-json]") {
    // Multi-byte text is copied through verbatim rather than escaped: the
    // canonical form is UTF-8, so a `\u` escape would be a second spelling of
    // the same character and two writers could disagree about the bytes.
    SECTION("each lead-byte class round-trips") {
        const auto passes = [](std::string_view raw) {
            INFO(raw.size() << " bytes");
            CHECK(quoted_or_fail(raw) == "{\"s\":\"" + std::string(raw) + "\"}");
        };
        passes("\xC3\xA9");          // U+00E9, two bytes
        passes("\xE0\xA0\x80");      // U+0800, the three-byte floor
        passes("\xE2\x82\xAC");      // U+20AC, the E1..EC run
        passes("\xED\x80\x80");      // U+D000, just below the surrogate block
        passes("\xEF\xBF\xBD");      // U+FFFD, the EE..EF run
        passes("\xF0\x90\x80\x80");  // U+10000, the four-byte floor
        passes("\xF1\x80\x80\x80");  // U+40000, the F1..F3 run
        passes("\xF4\x8F\xBF\xBF");  // U+10FFFF, the ceiling
    }

    SECTION("a malformed sequence is refused by class") {
        // A lead byte no encoding defines.
        expect_string_refused("\xFF", "an invalid lead byte");
        expect_string_refused("\xC0\x80", "an invalid lead byte");  // overlong two-byte
        expect_string_refused("\x80", "an invalid lead byte");      // continuation, unled
        // The declared length runs off the end of the string.
        expect_string_refused("\xE2\x82", "a truncated sequence");
        expect_string_refused("\xF0\x90\x80", "a truncated sequence");
        // The second byte is outside the range its lead byte allows. The
        // narrowed ranges are what bar an overlong form and the UTF-16
        // surrogates, which have no business in UTF-8 at all.
        expect_string_refused("\xE2\x20\xAC", "an invalid continuation byte");
        expect_string_refused("\xE0\x80\x80", "an invalid continuation byte");  // overlong
        expect_string_refused("\xED\xA0\x80", "an invalid continuation byte");  // U+D800
        expect_string_refused("\xF0\x80\x80\x80", "an invalid continuation byte");
        expect_string_refused("\xF4\x90\x80\x80", "an invalid continuation byte");  // past U+10FFFF
        // A later byte in the sequence is not a continuation.
        expect_string_refused("\xE2\x82\x20", "an invalid continuation byte");
        expect_string_refused("\xF0\x90\x80\x20", "an invalid continuation byte");
    }
}

TEST_CASE("a document with two members of one name is refused, not silently reduced",
          "[authoring-capsule][canonical-json]") {
    // A duplicate key makes the document mean whichever member a reader keeps,
    // so the digest would cover text two readers disagree about. It is refused
    // at the parse, before any canonical bytes exist.
    const auto refused = detail::parse_json("{\"a\":1,\"a\":2}", "/manifest");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().status == CapsuleStatus::manifest_invalid);
    CHECK(refused.error().subject == "/manifest");
    CHECK(refused.error().required == "parseable JSON");

    // Control: the same document with distinct names parses and canonicalizes,
    // so the refusal is about the repeated name and not about the shape.
    const auto distinct = detail::canonicalize_json_text("{\"b\":2,\"a\":1}", "/manifest");
    REQUIRE(distinct.has_value());
    CHECK(distinct.value() == "{\"a\":1,\"b\":2}");
}

TEST_CASE("nesting past the depth bound is refused rather than recursed into",
          "[authoring-capsule][canonical-json]") {
    // Both the parser and the serializer recurse, so an envelope of ten
    // thousand open brackets would exhaust the stack — a crash, not a
    // rejection. The bound is checked on the way in and on the way out.
    SECTION("while parsing") {
        const std::string too_deep = std::string(detail::kMaxJsonDepth + 1, '[') +
                                     std::string(detail::kMaxJsonDepth + 1, ']');
        const auto refused = detail::parse_json(too_deep, "/topology");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::manifest_invalid);
        CHECK(refused.error().required == "nesting within the depth bound");

        // Control: one level shallower parses, so the bound is the depth and
        // not the length.
        const std::string at_bound =
            std::string(detail::kMaxJsonDepth, '[') + std::string(detail::kMaxJsonDepth, ']');
        CHECK(detail::parse_json(at_bound, "/topology").has_value());
    }

    SECTION("while serializing") {
        // Built directly rather than parsed, so the serializer's own bound is
        // what refuses it rather than the prescan's.
        auto nested = choc::value::createEmptyArray();
        for (std::size_t i = 0; i < detail::kMaxJsonDepth + 2; ++i) {
            auto outer = choc::value::createEmptyArray();
            outer.addArrayElement(nested);
            nested = outer;
        }
        const auto refused = detail::to_canonical_text(nested.getView());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::manifest_invalid);
        CHECK(refused.error().required == "nesting within the depth bound");
    }
}

TEST_CASE("the prescan refuses what choc's parser would accept",
          "[authoring-capsule][canonical-json]") {
    const auto expect_refused = [](std::string_view text, std::string_view required,
                                   std::string_view found) {
        INFO(found);
        const auto refused = detail::parse_json(text, "/manifest");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().status == CapsuleStatus::manifest_invalid);
        CHECK(refused.error().subject == "/manifest");
        CHECK(refused.error().required == required);
        CHECK(refused.error().found == found);
    };

    // choc hands its number scanner a pointer it reads to a NUL terminator, so
    // an embedded NUL in bytes that came from an archive is a read past the
    // value.
    expect_refused(std::string_view("{\"a\":1\0}", 8), "text without NUL bytes",
                   "an embedded NUL");

    // choc's parseTopLevel returns as soon as the outermost value closes, so a
    // second document appended to the first would be silently ignored — and
    // the bytes a signature covers would not be the bytes that were parsed.
    expect_refused("{\"a\":1} {\"b\":2}", "one JSON value", "trailing content");
    expect_refused("{\"a\":1}x", "one JSON value", "trailing content");

    expect_refused("{\"a\":1]", "balanced brackets", "a mismatched close");
    expect_refused("[1,2}", "balanced brackets", "a mismatched close");
    expect_refused("}", "balanced brackets", "an unopened close");

    // A bracket inside a string is text, not structure — including one behind
    // an escaped quote or an escaped backslash, which is what the prescan's
    // own escape tracking is for.
    CHECK(detail::parse_json("{\"a\":\"}]{[\"}", "/manifest").has_value());
    CHECK(detail::parse_json("{\"a\":\"x\\\"}\"}", "/manifest").has_value());
    CHECK(detail::parse_json("{\"a\":\"x\\\\\"}", "/manifest").has_value());

    // Trailing whitespace after the outermost value is not trailing content.
    CHECK(detail::parse_json("{\"a\":1}  \n\t\r", "/manifest").has_value());
}

TEST_CASE("a document choc cannot parse is reported with where it stopped",
          "[authoring-capsule][canonical-json]") {
    const auto refused = detail::parse_json("{\"a\":}", "/manifest");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().status == CapsuleStatus::manifest_invalid);
    CHECK(refused.error().subject == "/manifest");
    CHECK(refused.error().required == "parseable JSON");
    // The position is what makes the message actionable; without it a person
    // is told only that a manifest they cannot read is unreadable.
    CHECK(refused.error().found.find("at line 1, column ") != std::string::npos);

    // Whitespace alone carries no value at all, which the prescan reports
    // before choc is handed anything to parse.
    const auto nothing = detail::parse_json("   ", "/manifest");
    REQUIRE_FALSE(nothing.has_value());
    CHECK(nothing.error().required == "a JSON object or array");
    CHECK(nothing.error().found == "no value");

    // canonicalize_json_text carries the parse failure out rather than
    // reporting a serialization problem for text that was never parsed.
    const auto propagated = detail::canonicalize_json_text("{\"a\":}", "/provenance");
    REQUIRE_FALSE(propagated.has_value());
    CHECK(propagated.error().subject == "/provenance");
    CHECK(propagated.error().required == "parseable JSON");
}

// ═══════════════════════════════════════════════════════════════════════════
// Manifest parsing — one pointer per field
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// The fields of a manifest this reader accepts, as key → raw JSON text, in
/// the order they are written. Building the document from a table is what lets
/// a case replace exactly one field and leave every other one well formed —
/// so a rejection can only be about the field that was changed.
std::vector<std::pair<std::string, std::string>> valid_manifest_fields() {
    return {
        {"format", "\"" + std::string(kFormatId) + "\""},
        {"format_version", "1"},
        {"profile", "\"com.example.capsule.test\""},
        {"profile_version", "1"},
        {"product", "\"test-product\""},
        {"authoring_kind", "\"instrument\""},
        {"subtypes", "[\"sampler\"]"},
        {"topology", "{\"voices\":1}"},
        {"required_capabilities", "[\"pcm-sample-bank\"]"},
        {"project_id", "\"project-42\""},
        {"revision_id", "\"\""},
        {"parent_revision", "\"\""},
        {"reproducibility", "\"best-effort\""},
        {"compatibility", "{\"min_product_version\":\"1.0.0\"}"},
        {"files", "[]"},
        {"dependencies", "[]"},
        {"title", "\"A Capsule\""},
        {"created_at", "\"2026-01-01T00:00:00Z\""},
        {"exported_at", "\"2026-01-02T03:04:05Z\""},
        {"provenance", "{\"tool\":\"pulp\"}"},
        {"attestations", "[]"},
        {"distribution", "{\"policy\":\"share-for-remix\"}"},
    };
}

std::string manifest_json(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) out += ',';
        out += '"';
        out += fields[i].first;
        out += "\":";
        out += fields[i].second;
    }
    out += '}';
    return out;
}

/// The valid document with one field's value replaced, or — when `value_json`
/// is empty — with that field removed entirely.
std::string manifest_json_with(std::string_view key, std::string_view value_json) {
    auto fields = valid_manifest_fields();
    std::vector<std::pair<std::string, std::string>> out;
    bool replaced = false;
    for (auto& field : fields) {
        if (field.first == key) {
            replaced = true;
            if (value_json.empty()) continue;
            field.second = std::string(value_json);
        }
        out.push_back(std::move(field));
    }
    // A key the table does not carry is a mistake in the test, not a case: it
    // would silently assert against the unmodified document.
    REQUIRE(replaced);
    return manifest_json(out);
}

void expect_manifest_invalid(std::string_view json, std::string_view pointer) {
    INFO(pointer);
    const auto parsed = parse_manifest(json);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().status == CapsuleStatus::manifest_invalid);
    CHECK(parsed.error().subject == pointer);
}

}  // namespace

TEST_CASE("every manifest field reports its own JSON pointer when it is malformed",
          "[authoring-capsule][manifest]") {
    // Control first: the assembled document parses. Without it every case
    // below would pass against a reader that rejected the fixture outright.
    const auto control = parse_manifest(manifest_json(valid_manifest_fields()));
    REQUIRE(control.has_value());
    CHECK(control->product == "test-product");
    CHECK(control->subtypes == std::vector<std::string>{"sampler"});

    // A pointer is what turns "your capsule is invalid" into a field a person
    // can open and fix, so each one is pinned to the field that produced it. A
    // reader that reported `/` for everything would satisfy a test that only
    // checked the status.
    SECTION("a string field holding something that is not a string") {
        for (const auto* key : {"format", "profile", "product", "authoring_kind", "project_id",
                                "revision_id", "parent_revision", "title", "created_at",
                                "exported_at"}) {
            expect_manifest_invalid(manifest_json_with(key, "5"), std::string("/") + key);
        }
    }

    SECTION("a required string field that is absent or empty") {
        expect_manifest_invalid(manifest_json_with("product", ""), "/product");
        expect_manifest_invalid(manifest_json_with("authoring_kind", "\"\""), "/authoring_kind");
    }

    SECTION("a version field that is not a whole number in range") {
        expect_manifest_invalid(manifest_json_with("format_version", "\"1\""), "/format_version");
        expect_manifest_invalid(manifest_json_with("format_version", "-1"), "/format_version");
        expect_manifest_invalid(manifest_json_with("format_version", "4294967296"),
                                "/format_version");
        // Absent is a different branch from present-but-wrong-typed, and both
        // have to name the same field.
        expect_manifest_invalid(manifest_json_with("format_version", ""), "/format_version");
        expect_manifest_invalid(manifest_json_with("profile_version", "\"1\""), "/profile_version");
        expect_manifest_invalid(manifest_json_with("profile_version", ""), "/profile_version");
    }

    SECTION("an array field holding something that is not an array of strings") {
        expect_manifest_invalid(manifest_json_with("subtypes", "5"), "/subtypes");
        expect_manifest_invalid(manifest_json_with("subtypes", "[5]"), "/subtypes/0");
        expect_manifest_invalid(manifest_json_with("required_capabilities", "5"),
                                "/required_capabilities");
        expect_manifest_invalid(manifest_json_with("required_capabilities", "[\"a\",7]"),
                                "/required_capabilities/1");
    }

    SECTION("a subtree field holding the wrong shape") {
        expect_manifest_invalid(manifest_json_with("topology", "5"), "/topology");
        expect_manifest_invalid(manifest_json_with("provenance", "[]"), "/provenance");
        expect_manifest_invalid(manifest_json_with("distribution", "5"), "/distribution");
        // `attestations` is the one that must be an array; an object there is
        // as wrong as an array is for the others.
        expect_manifest_invalid(manifest_json_with("attestations", "{}"), "/attestations");
    }

    SECTION("an enumerated field holding a token this build does not define") {
        expect_manifest_invalid(manifest_json_with("reproducibility", "\"perhaps\""),
                                "/reproducibility");
        expect_manifest_invalid(manifest_json_with("reproducibility", "5"), "/reproducibility");
    }

    SECTION("compatibility, whose members carry their own pointers") {
        expect_manifest_invalid(manifest_json_with("compatibility", "5"), "/compatibility");
        expect_manifest_invalid(
            manifest_json_with("compatibility", "{\"min_product_version\":5}"),
            "/compatibility/min_product_version");
        expect_manifest_invalid(
            manifest_json_with("compatibility", "{\"min_runtime_version\":[]}"),
            "/compatibility/min_runtime_version");
        expect_manifest_invalid(manifest_json_with("compatibility", "{\"schema_version\":5}"),
                                "/compatibility/schema_version");
    }

    SECTION("the row arrays, and the rows inside them") {
        expect_manifest_invalid(manifest_json_with("files", "5"), "/files");
        expect_manifest_invalid(manifest_json_with("files", "[5]"), "/files/0");
        expect_manifest_invalid(manifest_json_with("dependencies", "5"), "/dependencies");
        expect_manifest_invalid(manifest_json_with("dependencies", "[5]"), "/dependencies/0");
    }
}

TEST_CASE("a manifest field carries the version and shape rules past its own type check",
          "[authoring-capsule][manifest]") {
    // Zero is not a version this format ever had, so telling the author to
    // upgrade would be wrong advice — it is a malformed field.
    expect_manifest_invalid(manifest_json_with("format_version", "0"), "/format_version");
    expect_manifest_invalid(manifest_json_with("profile_version", "0"), "/profile_version");

    // A future format version is a different answer: the field is well formed
    // and the build is too old.
    const auto newer = parse_manifest(manifest_json_with(
        "format_version", std::to_string(static_cast<std::uint64_t>(kFormatVersion) + 1)));
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().status == CapsuleStatus::unsupported_format_version);
    CHECK(newer.error().subject == "/format_version");
    CHECK(newer.error().required == std::to_string(kFormatVersion));

    // And a document from another format entirely is told so, rather than
    // being walked field by field and reported as malformed.
    const auto foreign = parse_manifest(manifest_json_with("format", "\"com.example.other\""));
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error().status == CapsuleStatus::unsupported_format);
    CHECK(foreign.error().subject == "/format");
    CHECK(foreign.error().found == "com.example.other");
}

TEST_CASE("a file row's own fields are checked, each under the row's pointer",
          "[authoring-capsule][manifest]") {
    // The row is assembled from a table for the same reason the envelope is:
    // a case replaces exactly one member, so a rejection can only be about
    // that member. Appending a second copy of a key would instead produce a
    // duplicate name, which the parser refuses before any field is read.
    const auto row = [](std::string_view key, std::string_view value_json) {
        std::vector<std::pair<std::string, std::string>> members{
            {"role", "\"dsp.source\""},
            {"path", "\"dsp/main.cpp\""},
            {"sha256", "\"" + std::string(64, 'a') + "\""},
            {"bytes", "4"},
            {"media_type", "\"application/octet-stream\""},
            {"executable_data", "false"},
            {"policy",
             "{\"canonicality\":\"canonical-input\",\"source_availability\":\"included\","
             "\"editability\":\"editable\",\"disclosure\":\"public\","
             "\"redistribution\":\"allowed\"}"},
        };
        std::string out = "[{";
        bool first = true;
        bool replaced = key.empty();
        for (auto& member : members) {
            if (member.first == key) {
                replaced = true;
                if (value_json.empty()) continue;
                member.second = std::string(value_json);
            }
            if (!first) out += ',';
            first = false;
            out += '"';
            out += member.first;
            out += "\":";
            out += member.second;
        }
        REQUIRE(replaced);
        out += "}]";
        return out;
    };

    // Control: the row parses, so each rejection below is about the member it
    // replaced rather than about the fixture.
    const auto control = parse_manifest(manifest_json_with("files", row({}, {})));
    REQUIRE(control.has_value());
    REQUIRE(control->files.size() == 1);
    CHECK(control->files[0].path == "dsp/main.cpp");
    CHECK(control->files[0].bytes == 4);

    expect_manifest_invalid(manifest_json_with("files", row("role", "5")), "/files/0/role");
    expect_manifest_invalid(manifest_json_with("files", row("role", {})), "/files/0/role");
    expect_manifest_invalid(manifest_json_with("files", row("path", "5")), "/files/0/path");
    expect_manifest_invalid(manifest_json_with("files", row("sha256", "5")), "/files/0/sha256");
    expect_manifest_invalid(manifest_json_with("files", row("media_type", "5")),
                            "/files/0/media_type");
    expect_manifest_invalid(manifest_json_with("files", row("bytes", "\"4\"")), "/files/0/bytes");
    expect_manifest_invalid(manifest_json_with("files", row("bytes", "-1")), "/files/0/bytes");
    expect_manifest_invalid(manifest_json_with("files", row("bytes", {})), "/files/0/bytes");
    expect_manifest_invalid(manifest_json_with("files", row("executable_data", "5")),
                            "/files/0/executable_data");
    expect_manifest_invalid(manifest_json_with("files", row("policy", "5")), "/files/0/policy");
    expect_manifest_invalid(
        manifest_json_with("files", row("policy", "{\"canonicality\":\"invented\"}")),
        "/files/0/policy/canonicality");

    // A files[] row means the bytes travel in this archive, so any other
    // availability is a contradiction rather than a component nothing can
    // ever resolve.
    const auto external = parse_manifest(manifest_json_with(
        "files", row("policy",
                     "{\"canonicality\":\"canonical-input\","
                     "\"source_availability\":\"external\",\"editability\":\"editable\","
                     "\"disclosure\":\"public\",\"redistribution\":\"allowed\"}")));
    REQUIRE_FALSE(external.has_value());
    CHECK(external.error().status == CapsuleStatus::manifest_invalid);
    CHECK(external.error().subject == "/files/0/policy/source_availability");
    CHECK(external.error().required == "included");
    CHECK(external.error().found == "external");
}

TEST_CASE("an unknown descriptive key round-trips, and malformed text does not",
          "[authoring-capsule][manifest]") {
    // Unknown keys round-trip verbatim, which means their bytes reach the
    // digest — so a value with no canonical form must never be admitted. Text
    // that is not well-formed UTF-8 is refused at the parse, before any
    // canonical bytes exist.
    auto fields = valid_manifest_fields();
    fields.emplace_back("vendor_note", "\"\xFF\"");
    const auto refused = parse_manifest(manifest_json(fields));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().status == CapsuleStatus::manifest_invalid);

    // Control: the same key with well-formed text is preserved, in canonical
    // form, so a re-export reproduces it byte for byte.
    auto good = valid_manifest_fields();
    good.emplace_back("vendor_note", "\"one\"");
    const auto parsed = parse_manifest(manifest_json(good));
    REQUIRE(parsed.has_value());
    CHECK(parsed->unknown_optional_json == "{\"vendor_note\":\"one\"}");

    // A declared `completeness` is the one key that is consumed and dropped:
    // it is derived from the rows, and keeping it would let a stale claim
    // outlive the rows it was computed from.
    auto declared = valid_manifest_fields();
    declared.emplace_back("completeness", "\"self_contained\"");
    const auto ignored = parse_manifest(manifest_json(declared));
    REQUIRE(ignored.has_value());
    CHECK(ignored->unknown_optional_json.find("completeness") == std::string::npos);
}

TEST_CASE("profile-owned extras ride along under compatibility and provenance",
          "[authoring-capsule][manifest]") {
    // A floor this build does not know is carried, not dropped: it is part of
    // the identity, and a reader that discarded it would compute a different
    // revision than the writer did.
    auto fields = valid_manifest_fields();
    for (auto& field : fields) {
        if (field.first == "compatibility")
            field.second =
                "{\"min_runtime_version\":\"2.0.0\",\"forge_engine\":\"3\",\"b\":\"1\"}";
    }
    const auto parsed = parse_manifest(manifest_json(fields));
    REQUIRE(parsed.has_value());
    CHECK(parsed->compatibility.min_runtime_version == "2.0.0");
    // Canonical, so the extras contribute stable bytes regardless of the order
    // the writer emitted them in.
    CHECK(parsed->compatibility.extra_json == "{\"b\":\"1\",\"forge_engine\":\"3\"}");

    // And they survive the round trip back out.
    const auto canonical = to_canonical_json(*parsed);
    REQUIRE(canonical.has_value());
    CHECK(canonical.value().find("\"forge_engine\":\"3\"") != std::string::npos);

    // A manifest whose optional subtrees were never set still serializes: the
    // stored blob degrades to its empty shape rather than making the writer
    // fallible.
    Manifest bare = *parsed;
    bare.topology_json.clear();
    bare.provenance_json.clear();
    bare.attestations_json.clear();
    bare.distribution_json.clear();
    bare.compatibility.extra_json.clear();
    const auto emptied = to_canonical_json(bare);
    REQUIRE(emptied.has_value());
    CHECK(emptied.value().find("\"topology\":{}") != std::string::npos);
    CHECK(emptied.value().find("\"attestations\":[]") != std::string::npos);
}

TEST_CASE("a preview names every component that blocks a redistributable claim",
          "[authoring-capsule][preview][rights]") {
    // The blocker list is what a person reads before deciding whether they may
    // pass a capsule on, and the two row kinds live in different namespaces: a
    // file has an archive path, a dependency has a content identity. A
    // path-only list would be structurally empty for a profile whose blockers
    // are all dependencies — exactly the case where it has the most to say.
    TempDir temp;
    const fs::path destination = temp.path() / "mixed-rights.capsule";

    auto request = make_export_request();
    // One file row whose rights nobody granted, and one that is cleared, so
    // the list is a selection rather than a copy of every row.
    request.items[1].entry.policy.redistribution = Redistribution::unknown();
    request.manifest.dependencies = {dependency_row("sha256-zzz"), dependency_row("sha256-aaa")};
    request.manifest.dependencies[0].policy.redistribution = Redistribution::restricted();
    request.manifest.dependencies[1].policy.redistribution = Redistribution::unknown();

    REQUIRE(export_capsule(request, destination).has_value());
    auto archive = open_archive(destination);
    REQUIRE(archive.has_value());

    AdmissionOptions options;
    options.product = "test-product";
    const auto preview = preview_capsule(*archive, test_registry(), options);
    REQUIRE(preview.has_value());

    const auto& blocking = preview->rights.blocking_components;
    // Files sort before dependencies, and each kind sorts by its identifier,
    // so two previews of one capsule produce the same list whatever order the
    // rows arrived in.
    REQUIRE(blocking.size() == 3);
    CHECK(blocking[0] == ComponentRef::to_file("dsp/main.cpp"));
    CHECK(blocking[1] == ComponentRef::to_dependency("sha256-aaa"));
    CHECK(blocking[2] == ComponentRef::to_dependency("sha256-zzz"));
    CHECK(std::is_sorted(blocking.begin(), blocking.end()));

    // The row that was granted is absent, which is what proves the list is
    // selecting on rights rather than listing everything.
    CHECK(std::find(blocking.begin(), blocking.end(),
                    ComponentRef::to_file("audio/render.pcm")) == blocking.end());

    CHECK(preview->rights.any_unknown_redistribution);
    CHECK(preview->rights.any_restricted_redistribution);
    // Unknown rights anywhere mean this is not a capsule you may pass on.
    CHECK(preview->completeness != Completeness::self_contained);
}

TEST_CASE("a redistribution grant cannot be reached by omission",
          "[authoring-capsule][rights]") {
    // The type is what enforces this, not a comment: a grant is a claim about
    // someone else's work, and a permissive value that costs nothing to reach
    // is the one a writer reaches for without deciding anything.
    constexpr Redistribution defaulted{};
    static_assert(defaulted.is_unknown(), "a default-initialized grant must be unknown");
    static_assert(defaulted.state() == Redistribution::State::unknown);

    Redistribution value_initialized{};
    CHECK(value_initialized.state() == Redistribution::State::unknown);
    CHECK(value_initialized.is_unknown());
    CHECK_FALSE(value_initialized.is_granted());
    CHECK_FALSE(value_initialized.is_restricted());

    // Each named state reports itself, and only itself.
    CHECK(Redistribution::granted().state() == Redistribution::State::allowed);
    CHECK(Redistribution::granted().is_granted());
    CHECK(Redistribution::restricted().state() == Redistribution::State::restricted);
    CHECK(Redistribution::restricted().is_restricted());
    CHECK(Redistribution::unknown().state() == Redistribution::State::unknown);

    // A ComponentPolicy that never mentions redistribution is unknown too, so
    // a forgotten field cannot become a grant.
    const ComponentPolicy untouched;
    CHECK(untouched.redistribution.is_unknown());
}

TEST_CASE("a component reference is ordered by kind then identifier",
          "[authoring-capsule][rights]") {
    // The two kinds are identified in different namespaces, so a path and an
    // id that happen to spell the same string are different components. The
    // ordering has to see the kind first or a sorted list would interleave
    // them and dedup could fold one onto the other.
    const auto file = ComponentRef::to_file("same");
    const auto dependency = ComponentRef::to_dependency("same");
    CHECK(file.kind == ComponentRef::Kind::file);
    CHECK(dependency.kind == ComponentRef::Kind::dependency);
    CHECK(file.id == "same");
    CHECK_FALSE(file == dependency);
    CHECK(file < dependency);

    std::vector<ComponentRef> refs{ComponentRef::to_dependency("b"), ComponentRef::to_file("b"),
                                   ComponentRef::to_dependency("a"), ComponentRef::to_file("a")};
    std::sort(refs.begin(), refs.end());
    CHECK(refs[0] == ComponentRef::to_file("a"));
    CHECK(refs[1] == ComponentRef::to_file("b"));
    CHECK(refs[2] == ComponentRef::to_dependency("a"));
    CHECK(refs[3] == ComponentRef::to_dependency("b"));
}

TEST_CASE("moving a staging area hands the tree over exactly once",
          "[authoring-capsule][staging]") {
    TempDir temp;
    fs::path first_root;
    fs::path second_root;

    auto first = StagingArea::create(temp.path());
    REQUIRE(first.has_value());
    first_root = first->root();
    write_scratch_file(first_root / "a.bin", "first");

    auto second = StagingArea::create(temp.path());
    REQUIRE(second.has_value());
    second_root = second->root();
    REQUIRE(first_root != second_root);
    write_scratch_file(second_root / "b.bin", "second");

    // Move-assignment discards the destination's own tree before taking the
    // source's: leaving it behind would strand an owner-private directory
    // nothing will ever clean up.
    *first = std::move(*second);
    CHECK_FALSE(fs::exists(first_root));
    CHECK(fs::exists(second_root / "b.bin"));
    CHECK(first->root() == second_root);

    // A moved-from area owns nothing, and every entry point that takes one
    // refuses rather than joining a path to an empty root.
    FileEntry entry;
    entry.path = "b.bin";
    entry.bytes = 6;
    const auto read_back = read_staged_member(*second, entry);
    REQUIRE_FALSE(read_back.has_value());
    CHECK(read_back.error().status == CapsuleStatus::staging_failed);

    TempDir capsule_dir;
    auto fixture = open_fixture_capsule(capsule_dir.path() / "fixture.capsule");
    const auto extracted = extract_declared(fixture.archive, fixture.preview.manifest, *second);
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().status == CapsuleStatus::staging_failed);

    const auto published = second->publish_no_replace(temp.path() / "nowhere");
    REQUIRE_FALSE(published.has_value());
    CHECK(published.error().status == CapsuleStatus::staging_failed);
    CHECK_FALSE(fs::exists(temp.path() / "nowhere"));

    // Control: the area that received the tree still works, so the refusals
    // above are about the moved-from area and not about a broken fixture.
    const auto still_readable = read_staged_member(*first, entry);
    REQUIRE(still_readable.has_value());
    CHECK(std::string(still_readable->begin(), still_readable->end()) == "second");

    // Self-move-assignment must not discard the tree it is about to keep.
    StagingArea& alias = *first;
    *first = std::move(alias);
    CHECK(fs::exists(second_root / "b.bin"));
}

TEST_CASE("a declared digest is matched on its bytes, not on its spelling",
          "[authoring-capsule][staging]") {
    // A manifest spells a bare digest on a file row and a prefixed one on a
    // revision identity, and a hand-edited manifest may use uppercase hex.
    // None of those is a content difference, and reporting one as a mismatch
    // would send someone looking for tampering that never happened.
    TempDir temp;
    auto fixture = open_fixture_capsule(temp.path() / "fixture.capsule");

    Manifest manifest = fixture.preview.manifest;
    REQUIRE(manifest.files.size() == 2);
    manifest.files[0].sha256 = "sha256:" + manifest.files[0].sha256;
    auto& upper = manifest.files[1].sha256;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    auto staging = StagingArea::create(temp.path());
    REQUIRE(staging.has_value());
    REQUIRE(extract_declared(fixture.archive, manifest, *staging).has_value());
    CHECK(fs::exists(staging->root() / manifest.files[0].path));

    // Control: a digest that is the wrong length is still a mismatch, so the
    // tolerance above is about spelling and not about a comparison that
    // stopped comparing.
    Manifest truncated = fixture.preview.manifest;
    truncated.files[0].sha256 = truncated.files[0].sha256.substr(0, 32);
    auto second = StagingArea::create(temp.path());
    REQUIRE(second.has_value());
    const auto refused = extract_declared(fixture.archive, truncated, *second);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().status == CapsuleStatus::digest_mismatch);
    CHECK_FALSE(fs::exists(second->root() / truncated.files[0].path));
}
