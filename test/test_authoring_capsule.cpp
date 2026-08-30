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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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
    entry.policy = included_policy(Redistribution::allowed, {RequiredFor::rebuild});
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
    entry.policy = included_policy(Redistribution::allowed, {RequiredFor::play});
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
            included_policy(Redistribution::allowed, {RequiredFor::play, RequiredFor::rebuild});
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
            included_policy(Redistribution::unknown, {RequiredFor::play, RequiredFor::rebuild});
        manifest.files = {entry};
        const auto verdict = derive_completeness(manifest);
        CHECK(verdict != Completeness::self_contained);
        CHECK(verdict == Completeness::partial);

        // Same rows, `restricted` instead of `unknown`: also not self-contained.
        manifest.files.front().policy.redistribution = Redistribution::restricted;
        CHECK(derive_completeness(manifest) != Completeness::self_contained);

        // Control: flipping only the redistribution grant reaches
        // self_contained, so the two checks above are about rights and not
        // about some unrelated defect in the rows.
        manifest.files.front().policy.redistribution = Redistribution::allowed;
        CHECK(derive_completeness(manifest) == Completeness::self_contained);
    }

    SECTION("resolvable") {
        Manifest manifest;
        DependencyEntry entry = dependency_row("sha256-aaa");
        entry.policy =
            included_policy(Redistribution::allowed, {RequiredFor::play, RequiredFor::rebuild});
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
        rendition.policy = included_policy(Redistribution::unknown, {RequiredFor::play});
        rendition.policy.canonicality = Canonicality::derived_output;

        DependencyEntry source = dependency_row("sha256-source");
        source.policy = included_policy(Redistribution::unknown, {RequiredFor::rebuild});
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
        entry.policy = included_policy(Redistribution::allowed, {RequiredFor::play});
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
    CHECK(bytes.size() == original.samples.size() * sizeof(float));

    CanonicalPcmMedia media;
    media.channels = original.channels;
    media.sample_rate = original.sample_rate;
    media.frame_count = original.frame_count;

    const auto restored = from_canonical_bytes(bytes, media);
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
        CanonicalPcmMedia media;
        media.channels = 1;
        media.sample_rate = 48000;
        media.frame_count = 1;
        const auto restored = from_canonical_bytes(float32_data({nan}), media);
        REQUIRE_FALSE(restored.has_value());
        CHECK(restored.error().status == CapsuleStatus::decode_unsupported);
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

    // A container that is not RIFF/WAVE at all is named too, rather than
    // silently producing zero frames.
    const auto flac = decode_to_canonical(bytes_of("fLaC and then some padding bytes"));
    REQUIRE_FALSE(flac.has_value());
    CHECK(flac.error().status == CapsuleStatus::decode_unsupported);
    CHECK(flac.error().subject == "wav");
    CHECK(flac.error().found == "flac");
}

TEST_CASE("two renditions differing only in declared rate have different identities",
          "[authoring-capsule][canonical-pcm]") {
    const std::vector<float> samples{0.0f, 0.5f, -0.5f, 0.25f};
    const CanonicalPcm at_44100 = make_pcm(1, 44100, samples);
    const CanonicalPcm at_48000 = make_pcm(1, 48000, samples);

    REQUIRE(to_canonical_bytes(at_44100) == to_canonical_bytes(at_48000));
    // The rate is hashed alongside the samples precisely so identical bytes
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
    audio.entry.policy = included_policy(Redistribution::allowed, {RequiredFor::play});
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
    ungated.policy.redistribution = Redistribution::unknown;
    manifest.files.push_back(ungated);

    CHECK(derive_completeness(manifest) != Completeness::self_contained);

    // The same row with an explicit grant is fine: it is the unknown rights
    // that disqualify it, not the missing gate.
    manifest.files.front().policy.redistribution = Redistribution::allowed;
    CHECK(derive_completeness(manifest) == Completeness::self_contained);

    // `restricted` is disqualifying for the same reason, and neither ever
    // decays to `allowed`.
    manifest.files.front().policy.redistribution = Redistribution::restricted;
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
    audio.entry.policy = included_policy(Redistribution::allowed, {RequiredFor::play});
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
