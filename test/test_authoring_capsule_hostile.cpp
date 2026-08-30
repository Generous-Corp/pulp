/// @file test_authoring_capsule_hostile.cpp
/// The hostile archive corpus for the authoring-capsule substrate.
///
/// Every archive here is forged in memory by this file. A malicious capsule is
/// a container *shape*, so the forge below emits ZIP records field by field and
/// lets a case lie about exactly one thing — a compression method, a declared
/// expanded size, an external-attribute file type — and prove the reader
/// resolves that one lie to one exact `CapsuleStatus`. A checked-in binary
/// fixture would be an opaque blob nobody can review or vary.
///
/// Two disciplines run through the file, and both are load-bearing:
///
/// 1. Every rejection is paired with a control that must be ADMITTED. A reader
///    that refused every archive would pass a corpus made only of refusals, so
///    a refusal is evidence only next to an acceptance of the same shape with
///    the hostile field removed.
/// 2. Every rejection is followed by a filesystem check. A status is half the
///    contract; the other half is that nothing was written outside the process
///    and no staging tree survived.

#include <pulp/authoring_capsule/archive.hpp>
#include <pulp/authoring_capsule/capsule.hpp>
#include <pulp/authoring_capsule/limits.hpp>
#include <pulp/authoring_capsule/manifest.hpp>
#include <pulp/authoring_capsule/preview.hpp>
#include <pulp/authoring_capsule/profile_registry.hpp>
#include <pulp/authoring_capsule/safe_path.hpp>
#include <pulp/authoring_capsule/staging.hpp>
#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/zip.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac = pulp::authoring_capsule;
namespace fs = std::filesystem;

using ac::CapsuleError;
using ac::CapsuleLimits;
using ac::CapsuleStatus;
using ac::kCapsuleLimitsV1;
using ac::Manifest;

namespace {

// ── ZIP forge ───────────────────────────────────────────────────────────
//
// Deliberately not a general ZIP writer: every field a hostile archive needs to
// lie about is an explicit, optional override, and everything else is a
// constant. That keeps a case's diff from the well-formed control down to the
// single field under test.

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50u;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50u;
constexpr std::uint32_t kEndOfCentralDirSignature = 0x06054b50u;
constexpr std::size_t kLocalHeaderBytes = 30;
constexpr std::size_t kEndOfCentralDirBytes = 22;

/// A plain regular file with owner read/write, as a Unix-host writer records
/// it. Every hostile mode below is this value with its file-type nibble or a
/// permission bit changed.
constexpr std::uint32_t kRegularFileAttr = 0100644u << 16;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

/// Bitwise CRC-32/ISO-HDLC. A table would be faster and the corpus is tiny, so
/// the readable form wins.
std::uint32_t crc32_of(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0u ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

struct ForgedMember {
    /// Raw name bytes, written verbatim. Not a `fs::path` and not
    /// NUL-terminated, so a case can put a NUL, a backslash, or a traversal
    /// segment inside a member name.
    std::string name;
    /// The logical content. Also what the default CRC and the default declared
    /// expanded size are taken from.
    std::vector<std::uint8_t> content;
    /// Compress `content` with raw deflate and record method 8.
    bool deflate = false;
    /// Replace the bytes that actually land in the member, leaving the declared
    /// sizes and CRC describing `content`. This is how a truncated member and a
    /// bomb are expressed.
    std::optional<std::vector<std::uint8_t>> raw_payload;
    std::optional<std::uint16_t> method;
    std::uint16_t flags = 0;
    std::uint32_t external_attr = kRegularFileAttr;
    std::optional<std::uint32_t> declared_compressed;
    std::optional<std::uint32_t> declared_expanded;
    std::optional<std::uint32_t> crc;
};

/// A plain stored member with every override left alone.
ForgedMember plain_member(std::string name, std::vector<std::uint8_t> content) {
    ForgedMember member;
    member.name = std::move(name);
    member.content = std::move(content);
    return member;
}

std::vector<std::uint8_t> forge_zip(const std::vector<ForgedMember>& members) {
    std::vector<std::uint8_t> archive;
    std::vector<std::uint8_t> central;
    std::vector<std::uint32_t> offsets;

    for (const auto& member : members) {
        std::vector<std::uint8_t> payload = member.content;
        std::uint16_t method = 0;
        if (member.deflate) {
            auto compressed =
                pulp::runtime::deflate_compress(member.content.data(), member.content.size(), 6);
            REQUIRE(compressed.has_value());
            payload = std::move(*compressed);
            method = 8;
        }
        if (member.raw_payload)
            payload = *member.raw_payload;
        if (member.method)
            method = *member.method;

        const auto declared_compressed =
            member.declared_compressed.value_or(static_cast<std::uint32_t>(payload.size()));
        const auto declared_expanded =
            member.declared_expanded.value_or(static_cast<std::uint32_t>(member.content.size()));
        const auto crc = member.crc.value_or(crc32_of(member.content));

        offsets.push_back(static_cast<std::uint32_t>(archive.size()));

        put_u32(archive, kLocalHeaderSignature);
        put_u16(archive, 20);
        put_u16(archive, member.flags);
        put_u16(archive, method);
        put_u16(archive, 0);       // fixed DOS time
        put_u16(archive, 0x0021);  // fixed DOS date, 1980-01-01
        put_u32(archive, crc);
        put_u32(archive, declared_compressed);
        put_u32(archive, declared_expanded);
        put_u16(archive, static_cast<std::uint16_t>(member.name.size()));
        put_u16(archive, 0);  // no extra field
        archive.insert(archive.end(), member.name.begin(), member.name.end());
        archive.insert(archive.end(), payload.begin(), payload.end());

        put_u32(central, kCentralHeaderSignature);
        put_u16(central, 0x0314);  // Unix host, ZIP 2.0
        put_u16(central, 20);
        put_u16(central, member.flags);
        put_u16(central, method);
        put_u16(central, 0);
        put_u16(central, 0x0021);
        put_u32(central, crc);
        put_u32(central, declared_compressed);
        put_u32(central, declared_expanded);
        put_u16(central, static_cast<std::uint16_t>(member.name.size()));
        put_u16(central, 0);  // extra field length
        put_u16(central, 0);  // comment length
        put_u16(central, 0);  // disk number start
        put_u16(central, 0);  // internal attributes
        put_u32(central, member.external_attr);
        put_u32(central, offsets.back());
        central.insert(central.end(), member.name.begin(), member.name.end());
    }

    const auto central_offset = static_cast<std::uint32_t>(archive.size());
    archive.insert(archive.end(), central.begin(), central.end());

    std::vector<std::uint8_t> tail;
    put_u32(tail, kEndOfCentralDirSignature);
    put_u16(tail, 0);
    put_u16(tail, 0);
    put_u16(tail, static_cast<std::uint16_t>(members.size()));
    put_u16(tail, static_cast<std::uint16_t>(members.size()));
    put_u32(tail, static_cast<std::uint32_t>(central.size()));
    put_u32(tail, central_offset);
    put_u16(tail, 0);
    REQUIRE(tail.size() == kEndOfCentralDirBytes);
    archive.insert(archive.end(), tail.begin(), tail.end());

    // A local header is a fixed-width record; a forge that got a field width
    // wrong would misplace every offset and produce a container miniz rejects
    // for reasons that have nothing to do with the case under test.
    REQUIRE(offsets.front() == 0u);
    REQUIRE(archive.size() >= kLocalHeaderBytes + kEndOfCentralDirBytes);
    return archive;
}

// ── Sandbox ─────────────────────────────────────────────────────────────

/// A private directory plus a recursive snapshot, so "nothing was written
/// outside the process" is an assertion rather than a claim.
class Sandbox {
public:
    Sandbox() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = fs::temp_directory_path();
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto candidate =
                base / ("pulp-capsule-hostile-" + std::to_string(stamp) + "-" +
                        std::to_string(counter.fetch_add(1)) + "-" + std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                root_ = std::move(candidate);
                return;
            }
        }
        FAIL("could not create a sandbox directory");
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    ~Sandbox() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    const fs::path& root() const noexcept { return root_; }

    /// Every entry below the root, relative and typed, sorted. Type is included
    /// so a file replaced by a directory of the same name is a difference.
    std::vector<std::string> snapshot() const {
        std::vector<std::string> entries;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied,
                                                 ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            const auto relative = fs::relative(it->path(), root_, ec).generic_string();
            entries.push_back((it->is_directory(ec) ? "dir:" : "file:") + relative);
        }
        std::sort(entries.begin(), entries.end());
        return entries;
    }

    fs::path write(std::string_view name, const std::vector<std::uint8_t>& bytes) const {
        const auto target = root_ / name;
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        out.close();
        REQUIRE(fs::exists(target));
        return target;
    }

private:
    fs::path root_;
};

// ── Payloads and manifests ──────────────────────────────────────────────

constexpr std::string_view kTestProfile = "org.pulp.test.hostile-corpus";
constexpr std::string_view kTestProduct = "pulp-hostile-corpus";

/// Deterministic, effectively incompressible bytes. Used wherever a case needs
/// a payload whose expansion ratio is about 1:1, so the ratio ceiling does not
/// fire before the property under test does.
std::vector<std::uint8_t> noise(std::size_t count, std::uint32_t seed) {
    std::vector<std::uint8_t> out;
    out.reserve(count);
    std::uint32_t state = seed | 1u;
    for (std::size_t index = 0; index < count; ++index) {
        state = state * 1664525u + 1013904223u;
        out.push_back(static_cast<std::uint8_t>((state >> 24) & 0xFFu));
    }
    return out;
}

std::string digest_of(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::uint8_t kNone[1] = {0};
    return pulp::runtime::sha256_hex(bytes.empty() ? kNone : bytes.data(), bytes.size());
}

struct Payload {
    std::string path;
    std::vector<std::uint8_t> bytes;
};

Manifest manifest_for(const std::vector<Payload>& payloads) {
    Manifest manifest;
    manifest.profile = std::string(kTestProfile);
    manifest.profile_version = 1;
    manifest.product = std::string(kTestProduct);
    manifest.authoring_kind = "test";
    manifest.project_id = "project-hostile-corpus";
    manifest.title = "hostile corpus";
    manifest.created_at = "2026-01-01T00:00:00Z";
    for (const auto& payload : payloads) {
        ac::FileEntry entry;
        entry.role = "test.payload";
        entry.path = payload.path;
        entry.sha256 = digest_of(payload.bytes);
        entry.bytes = payload.bytes.size();
        entry.media_type = "application/octet-stream";
        entry.policy.source_availability = ac::SourceAvailability::included;
        entry.policy.redistribution = ac::Redistribution::allowed;
        manifest.files.push_back(std::move(entry));
    }
    return manifest;
}

/// Stamp the recomputed identity and serialize. Every mutation a case makes to
/// the manifest goes in BEFORE this, so the capsule is internally consistent
/// and the only lie is the one the case is testing.
std::vector<std::uint8_t> seal(Manifest& manifest) {
    auto digest = ac::revision_digest(manifest);
    REQUIRE(digest);
    manifest.revision_id = *digest;
    auto json = ac::to_canonical_json(manifest);
    REQUIRE(json);
    return std::vector<std::uint8_t>(json->begin(), json->end());
}

/// Manifest member first, then the payloads in the given order.
std::vector<ForgedMember> members_for(const std::vector<std::uint8_t>& manifest_bytes,
                                      const std::vector<Payload>& payloads) {
    std::vector<ForgedMember> members;
    members.push_back(plain_member(std::string(ac::kManifestPath), manifest_bytes));
    for (const auto& payload : payloads)
        members.push_back(plain_member(payload.path, payload.bytes));
    return members;
}

/// A well-formed capsule: the control every hostile variant is a one-field
/// mutation of.
std::vector<std::uint8_t> well_formed_capsule(const std::vector<Payload>& payloads) {
    auto manifest = manifest_for(payloads);
    const auto manifest_bytes = seal(manifest);
    return forge_zip(members_for(manifest_bytes, payloads));
}

// ── Profile ─────────────────────────────────────────────────────────────

class CorpusProfile final : public ac::ProfileValidator {
public:
    std::string_view profile_id() const noexcept override { return kTestProfile; }
    std::uint32_t max_profile_version() const noexcept override { return 1; }
    std::vector<std::string> required_roles() const override { return {"test.payload"}; }
    bool supports_capability(std::string_view) const noexcept override { return true; }
    pulp::runtime::Result<void, CapsuleError> check_compatibility(const Manifest&) const override {
        return {};
    }
    pulp::runtime::Result<void, CapsuleError> validate_staged(const Manifest&,
                                                              const fs::path&) const override {
        return {};
    }
};

ac::ProfileRegistry corpus_registry() {
    ac::ProfileRegistry registry;
    registry.register_profile(std::make_shared<CorpusProfile>());
    return registry;
}

ac::AdmissionOptions corpus_options(const CapsuleLimits& limits = kCapsuleLimitsV1) {
    ac::AdmissionOptions options;
    options.limits = limits;
    options.product = std::string(kTestProduct);
    return options;
}

// ── Assertion helpers ───────────────────────────────────────────────────

struct OpenOutcome {
    CapsuleStatus status = CapsuleStatus::ok;
    std::string subject;
    std::string required;
};

OpenOutcome open_status(const fs::path& file, const CapsuleLimits& limits = kCapsuleLimitsV1) {
    auto archive = ac::open_archive(file, limits);
    if (archive)
        return OpenOutcome{CapsuleStatus::ok, {}, {}};
    const auto& error = archive.error();
    return OpenOutcome{error.status, error.subject, error.required};
}

/// Write, open, and report — with the sandbox proven unchanged apart from the
/// capsule file itself. Admission must not create a scratch file, a staging
/// directory, or a partial extraction anywhere.
OpenOutcome admit(const Sandbox& sandbox, const std::vector<std::uint8_t>& archive_bytes,
                  const CapsuleLimits& limits = kCapsuleLimitsV1) {
    const auto file = sandbox.write("capsule.forged", archive_bytes);
    const auto before = sandbox.snapshot();
    const auto outcome = open_status(file, limits);
    CHECK(sandbox.snapshot() == before);
    std::error_code ignored;
    fs::remove(file, ignored);
    return outcome;
}

struct HostileCase {
    std::string name;
    std::vector<ForgedMember> members;
    CapsuleStatus expected = CapsuleStatus::unsafe_archive;
    CapsuleLimits limits = kCapsuleLimitsV1;
};

void run_cases(const Sandbox& sandbox, const std::vector<HostileCase>& cases) {
    for (const auto& hostile : cases) {
        INFO("case: " << hostile.name);
        const auto outcome = admit(sandbox, forge_zip(hostile.members), hostile.limits);
        INFO("status: " << ac::status_token(outcome.status)
                        << " expected: " << ac::status_token(hostile.expected)
                        << " subject: " << outcome.subject << " rule: " << outcome.required);
        CHECK(outcome.status == hostile.expected);
    }
}

std::vector<ForgedMember> one_payload_archive(const std::string& member_name) {
    // The manifest content is irrelevant to a container-shape rejection: every
    // case in this family fails before the manifest is ever parsed. Keeping it
    // minimal keeps the archive small and the failure attributable.
    const std::string manifest = "{}";
    std::vector<ForgedMember> members;
    members.push_back(plain_member(std::string(ac::kManifestPath),
                                   std::vector<std::uint8_t>(manifest.begin(), manifest.end())));
    members.push_back(plain_member(member_name, noise(64, 7)));
    return members;
}

}  // namespace

// ── The control ─────────────────────────────────────────────────────────

TEST_CASE("a well-formed forged capsule is admitted end to end", "[authoring-capsule][hostile]") {
    // This is the control the whole file rests on. Without it, every rejection
    // below could be the forge emitting a container no reader would accept,
    // and a corpus of refusals would look identical to a corpus of successes.
    Sandbox sandbox;
    const std::vector<Payload> payloads{{"dsp/source.js", noise(512, 1)},
                                        {"audio/canonical.pcm", noise(2048, 2)}};
    const auto file = sandbox.write("capsule.forged", well_formed_capsule(payloads));

    auto archive = ac::open_archive(file);
    REQUIRE(archive);
    REQUIRE(archive->members().size() == 3);
    CHECK(archive->members()[0].path == ac::kManifestPath);

    const auto registry = corpus_registry();
    auto preview = ac::preview_capsule(*archive, registry, corpus_options());
    REQUIRE(preview);
    CHECK(preview->compatibility == ac::CompatibilityVerdict::supported);
    CHECK(preview->member_count == 3);
    CHECK(preview->unmet.status == CapsuleStatus::ok);
    CHECK_FALSE(preview->signature_verified);

    auto staging = ac::StagingArea::create(sandbox.root());
    REQUIRE(staging);
    const auto staging_root = staging->root();
    REQUIRE(ac::admit_to_staging(*archive, *preview, registry, *staging));
    CHECK(fs::exists(staging_root / "dsp" / "source.js"));
    CHECK(fs::exists(staging_root / "audio" / "canonical.pcm"));

    const auto destination = sandbox.root() / "published";
    REQUIRE(staging->publish_no_replace(destination));
    CHECK(fs::exists(destination / "dsp" / "source.js"));
}

// ── Paths ───────────────────────────────────────────────────────────────

TEST_CASE("hostile member paths are rejected at admission", "[authoring-capsule][hostile][path]") {
    Sandbox sandbox;

    std::string nul_name = "data/x.bin";
    nul_name[4] = '\0';  // an embedded NUL, not a terminator

    std::string deep = "d0";
    for (int level = 1; level <= 32; ++level)
        deep += "/d" + std::to_string(level);  // 33 components against a depth budget of 32

    const std::string overlong = std::string(kCapsuleLimitsV1.max_path_bytes + 1, 'a');

    std::vector<HostileCase> cases;
    const auto add = [&cases](std::string name, std::string member,
                              CapsuleStatus expected = CapsuleStatus::path_rejected) {
        cases.push_back(HostileCase{std::move(name), one_payload_archive(std::move(member)),
                                    expected, kCapsuleLimitsV1});
    };

    add("parent traversal", "../escape.bin");
    add("nested traversal", "data/../../escape.bin");
    add("current-directory component", "./escape.bin");
    add("absolute posix path", "/etc/passwd");
    add("drive letter", "C:/windows/system32/escape.bin");
    add("lowercase drive letter", "c:escape.bin");
    add("unc prefix", "//server/share/escape.bin");
    add("backslash unc prefix", "\\\\server\\share\\escape.bin");
    add("backslash separator", "data\\escape.bin");
    add("embedded nul", nul_name);
    add("c0 control", std::string("data/\x01" "escape.bin"));
    add("del control", std::string("data/\x7F" "escape.bin"));
    add("excessive depth", deep + "/leaf.bin");
    add("excessive path bytes", overlong);
    add("empty component", "data//escape.bin");
    add("leading separator", "/data/escape.bin");
    add("trailing dot", "data/escape.");
    add("trailing space", "data/escape ");
    add("windows reserved device name", "data/aux.bin");
    add("windows reserved character", "data/es<cape.bin");
    add("colon", "data/es:cape.bin");
    add("ill-formed utf-8", std::string("data/\xC3\x28.bin"));
    add("overlong utf-8 encoding", std::string("data/\xC0\xAF.bin"));
    add("surrogate half", std::string("data/\xED\xA0\x80.bin"));

    run_cases(sandbox, cases);

    // The control. Non-ASCII is not the problem: a precomposed name drawn from
    // the provably-NFC subset is admitted, so every rejection above is about
    // the hostile construct and not about a reader that refuses anything odd.
    const auto benign = one_payload_archive("data/caf\xC3\xA9.bin");
    CHECK(admit(sandbox, forge_zip(benign)).status == CapsuleStatus::ok);
}

TEST_CASE("colliding member names are rejected", "[authoring-capsule][hostile][path]") {
    Sandbox sandbox;

    const auto pair_archive = [](const std::string& first, const std::string& second) {
        const std::string manifest = "{}";
        std::vector<ForgedMember> members;
        members.push_back(plain_member(
            std::string(ac::kManifestPath),
            std::vector<std::uint8_t>(manifest.begin(), manifest.end())));
        members.push_back(plain_member(first, noise(32, 11)));
        members.push_back(plain_member(second, noise(32, 12)));
        return members;
    };

    std::vector<HostileCase> cases{
        {"duplicate member name", pair_archive("data/a.bin", "data/a.bin"),
         CapsuleStatus::path_collision, kCapsuleLimitsV1},
        {"ascii case collision", pair_archive("data/A.bin", "data/a.bin"),
         CapsuleStatus::path_collision, kCapsuleLimitsV1},
        {"ascii case collision in a directory component",
         pair_archive("Data/a.bin", "data/a.bin"), CapsuleStatus::path_collision,
         kCapsuleLimitsV1},
        {"latin-1 case collision", pair_archive("data/\xC3\x89.bin", "data/\xC3\xA9.bin"),
         CapsuleStatus::path_collision, kCapsuleLimitsV1},
        // `l`, `1`, and `i` fold to one representative: a person reading a file
        // list cannot tell them apart, so on the receiving machine one member
        // would overwrite the other while the closure still looked satisfied.
        {"confusable digit-for-letter collision", pair_archive("data/l1.bin", "data/11.bin"),
         CapsuleStatus::path_collision, kCapsuleLimitsV1},
        {"confusable zero-for-o collision", pair_archive("data/f0o.bin", "data/foo.bin"),
         CapsuleStatus::path_collision, kCapsuleLimitsV1},
        // A normalization collision cannot reach the collision check, because
        // the decomposed spelling carries a combining mark and the path gate
        // admits only code points it can prove are already NFC. The pair is
        // still refused; it is refused one step earlier, and `path_rejected`
        // names the reason precisely — the substrate links no character
        // database, so it can prove NFC or refuse, never normalize.
        {"unicode normalization collision",
         pair_archive("data/caf\xC3\xA9.bin", "data/cafe\xCC\x81.bin"),
         CapsuleStatus::path_rejected, kCapsuleLimitsV1},
    };

    run_cases(sandbox, cases);

    // Controls. Two distinct names in the same directory are admitted, and so
    // is the precomposed half of the normalization pair on its own — so the
    // collision verdicts above are about the pairing, not about either name.
    CHECK(admit(sandbox, forge_zip(pair_archive("data/a.bin", "data/b.bin"))).status ==
          CapsuleStatus::ok);
    CHECK(admit(sandbox, forge_zip(pair_archive("data/caf\xC3\xA9.bin", "data/b.bin"))).status ==
          CapsuleStatus::ok);
}

// ── Member modes, encryption, and compression methods ───────────────────

TEST_CASE("special members and unsupported encodings are refused",
          "[authoring-capsule][hostile][member]") {
    Sandbox sandbox;

    const auto with_attr = [](std::uint32_t attr) {
        auto members = one_payload_archive("data/payload.bin");
        members[1].external_attr = attr;
        return members;
    };
    const auto with_flags = [](std::uint16_t flags) {
        auto members = one_payload_archive("data/payload.bin");
        members[1].flags = flags;
        return members;
    };
    const auto with_method = [](std::uint16_t method) {
        auto members = one_payload_archive("data/payload.bin");
        // A non-store method must not also trip the store-size rule miniz
        // enforces on the central directory, so the declared sizes are left
        // describing the same bytes and only the method is a lie.
        members[1].method = method;
        members[1].declared_compressed = static_cast<std::uint32_t>(members[1].content.size());
        members[1].declared_expanded = static_cast<std::uint32_t>(members[1].content.size());
        return members;
    };

    std::vector<HostileCase> cases{
        // ZIP records a member's file type in the high half of the external
        // attributes. Anything that is not a regular file could reach outside
        // the staging directory, or be a node the extractor would have to
        // create with elevated meaning.
        //
        // A hardlink has no distinct encoding here — ZIP describes a hardlinked
        // file with exactly the regular-file mode of its target — so it is
        // covered the only way it can be: the rule is an allowlist of one
        // (`S_IFREG`), and every other type in the nibble is enumerated below,
        // rather than a blocklist that a type nobody thought of slips past.
        {"symlink member", with_attr(0120777u << 16), CapsuleStatus::unsafe_archive},
        {"fifo member", with_attr(0010644u << 16), CapsuleStatus::unsafe_archive},
        {"character device member", with_attr(0020644u << 16), CapsuleStatus::unsafe_archive},
        {"block device member", with_attr(0060644u << 16), CapsuleStatus::unsafe_archive},
        {"socket member", with_attr(0140644u << 16), CapsuleStatus::unsafe_archive},
        {"unknown file type", with_attr(0030644u << 16), CapsuleStatus::unsafe_archive},
        {"setuid member", with_attr(0104755u << 16), CapsuleStatus::unsafe_archive},
        {"setgid member", with_attr(0102755u << 16), CapsuleStatus::unsafe_archive},
        {"sticky member", with_attr(0101755u << 16), CapsuleStatus::unsafe_archive},
        {"dos directory attribute", with_attr(kRegularFileAttr | 0x10u),
         CapsuleStatus::unsafe_archive},
        {"dos volume label attribute", with_attr(kRegularFileAttr | 0x08u),
         CapsuleStatus::unsafe_archive},

        // A directory member can never satisfy the closure, because a
        // `files[]` row describes bytes and a directory has none.
        {"directory member", one_payload_archive("data/"), CapsuleStatus::unsafe_archive},

        // A capsule carries no key material, so an encrypted member is one the
        // reader could never honestly expand.
        {"encrypted member", with_flags(0x0001), CapsuleStatus::unsafe_archive},
        {"strongly encrypted member", with_flags(0x0040), CapsuleStatus::unsafe_archive},
        {"compressed-patch member", with_flags(0x0020), CapsuleStatus::unsafe_archive},
        {"masked local header", with_flags(0x2000), CapsuleStatus::unsafe_archive},

        // Only store and deflate. Every other method either needs a decoder the
        // substrate does not carry or, like method 99, hides encryption.
        {"bzip2 method", with_method(12), CapsuleStatus::unsafe_archive},
        {"lzma method", with_method(14), CapsuleStatus::unsafe_archive},
        {"zstd method", with_method(93), CapsuleStatus::unsafe_archive},
        {"aes method", with_method(99), CapsuleStatus::unsafe_archive},
        {"shrink method", with_method(1), CapsuleStatus::unsafe_archive},
    };

    run_cases(sandbox, cases);

    // Controls: the two shapes a legitimate writer produces. A Unix-host
    // regular file, and the mode-0 external attributes an MS-DOS-host writer
    // records — the reader must not mistake "no mode recorded" for a special
    // file.
    CHECK(admit(sandbox, forge_zip(with_attr(kRegularFileAttr))).status == CapsuleStatus::ok);
    CHECK(admit(sandbox, forge_zip(with_attr(0u))).status == CapsuleStatus::ok);
    {
        auto deflated = one_payload_archive("data/payload.bin");
        deflated[1].deflate = true;
        CHECK(admit(sandbox, forge_zip(deflated)).status == CapsuleStatus::ok);
    }
}

// ── Budgets ─────────────────────────────────────────────────────────────

TEST_CASE("a member that inflates past its declared size is refused",
          "[authoring-capsule][hostile][budget]") {
    // The header's expanded size is written by whoever built the archive, so it
    // is a claim rather than a fact. This is the case that proves the reader
    // counts the bytes coming out of the inflater instead of trusting it.
    Sandbox sandbox;

    constexpr std::size_t kRealSize = 100000;
    constexpr std::uint32_t kDeclaredSize = 64;
    const auto real = noise(kRealSize, 31);

    const std::vector<Payload> payloads{{"data/bomb.bin", real}};
    auto manifest = manifest_for(payloads);
    const auto manifest_bytes = seal(manifest);

    auto honest = members_for(manifest_bytes, payloads);
    honest[1].deflate = true;

    auto bomb = honest;
    bomb[1].declared_expanded = kDeclaredSize;

    // The honest control first: the same deflate stream, described truthfully,
    // reads back in full. Without it, the refusal below could be the reader
    // failing to inflate this stream at all.
    std::uint64_t honest_peak = 0;
    {
        const auto file = sandbox.write("honest.forged", forge_zip(honest));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        auto expanded = archive->read("data/bomb.bin");
        REQUIRE(expanded);
        CHECK(expanded->size() == kRealSize);
        // Compared as a bool: a byte-vector comparison Catch2 could stringify
        // would dump 100 kB into the report on any failure, and on `-s` even
        // on success.
        CHECK((*expanded == real));
        honest_peak = archive->peak_bytes();
        CHECK(honest_peak >= kRealSize);
    }

    const auto file = sandbox.write("bomb.forged", forge_zip(bomb));
    const auto before = sandbox.snapshot();
    auto archive = ac::open_archive(file);
    // The container shape is legal: a 64-byte member is inside every budget.
    // Only expansion can catch this one, which is the point.
    REQUIRE(archive);
    CHECK(archive->members()[1].expanded_bytes == kDeclaredSize);

    auto expanded = archive->read("data/bomb.bin");
    REQUIRE(expanded.is_err());
    CHECK(expanded.error().status == CapsuleStatus::archive_budget_exceeded);
    CHECK(expanded.error().subject == "data/bomb.bin");

    // The differential that proves the real inflated size never materialized.
    // The absolute peak is not the evidence: it is dominated by the inflater's
    // fixed machinery — the dictionary window, the decompressor state, the
    // stdio buffer — which both reads pay alike. What separates them is the
    // output charge, and the honest read carries a whole extra `kRealSize` of
    // it. A reader that trusted the header would have charged the same.
    const std::uint64_t bomb_peak = archive->peak_bytes();
    INFO("honest peak " << honest_peak << " vs bomb peak " << bomb_peak);
    CHECK(bomb_peak < honest_peak);
    CHECK(honest_peak - bomb_peak >= kRealSize / 2);
    CHECK(sandbox.snapshot() == before);
}

TEST_CASE("archive budgets fail closed with a boundary control",
          "[authoring-capsule][hostile][budget]") {
    Sandbox sandbox;

    SECTION("member count") {
        const std::string manifest = "{}";
        std::vector<ForgedMember> members;
        members.push_back(plain_member(
            std::string(ac::kManifestPath),
            std::vector<std::uint8_t>(manifest.begin(), manifest.end())));
        for (int index = 0; index < 5; ++index)
            members.push_back(
                plain_member("data/m" + std::to_string(index) + ".bin", noise(16, 40 + index)));
        const auto bytes = forge_zip(members);

        CapsuleLimits tight = kCapsuleLimitsV1;
        tight.max_members = members.size() - 1;
        CHECK(admit(sandbox, bytes, tight).status == CapsuleStatus::archive_budget_exceeded);

        CapsuleLimits exact = kCapsuleLimitsV1;
        exact.max_members = members.size();
        CHECK(admit(sandbox, bytes, exact).status == CapsuleStatus::ok);
    }

    SECTION("single member expanded bytes") {
        const std::vector<Payload> payloads{{"data/big.bin", noise(4096, 41)}};
        const auto bytes = well_formed_capsule(payloads);

        CapsuleLimits tight = kCapsuleLimitsV1;
        tight.max_member_expanded_bytes = 4095;
        const auto refused = admit(sandbox, bytes, tight);
        CHECK(refused.status == CapsuleStatus::archive_budget_exceeded);
        // Attributed to the oversized payload, not to the manifest that shares
        // the archive with it.
        CHECK(refused.subject == "data/big.bin");

        CapsuleLimits exact = kCapsuleLimitsV1;
        exact.max_member_expanded_bytes = 4096;
        CHECK(admit(sandbox, bytes, exact).status == CapsuleStatus::ok);
    }

    SECTION("total expanded bytes") {
        const std::vector<Payload> payloads{{"data/a.bin", noise(2048, 42)},
                                            {"data/b.bin", noise(2048, 43)}};
        auto manifest = manifest_for(payloads);
        const auto manifest_bytes = seal(manifest);
        const auto bytes = forge_zip(members_for(manifest_bytes, payloads));
        const std::uint64_t total = manifest_bytes.size() + 2048 + 2048;

        CapsuleLimits tight = kCapsuleLimitsV1;
        tight.max_expanded_bytes = total - 1;
        CHECK(admit(sandbox, bytes, tight).status == CapsuleStatus::archive_budget_exceeded);

        CapsuleLimits exact = kCapsuleLimitsV1;
        exact.max_expanded_bytes = total;
        CHECK(admit(sandbox, bytes, exact).status == CapsuleStatus::ok);
    }

    SECTION("compressed archive bytes") {
        const std::vector<Payload> payloads{{"data/a.bin", noise(2048, 44)}};
        const auto bytes = well_formed_capsule(payloads);

        CapsuleLimits tight = kCapsuleLimitsV1;
        tight.max_compressed_bytes = bytes.size() - 1;
        CHECK(admit(sandbox, bytes, tight).status == CapsuleStatus::archive_budget_exceeded);

        CapsuleLimits exact = kCapsuleLimitsV1;
        exact.max_compressed_bytes = bytes.size();
        CHECK(admit(sandbox, bytes, exact).status == CapsuleStatus::ok);
    }

    SECTION("single member expansion ratio") {
        // 100 000 zero bytes deflate to a few hundred, which is far past the
        // 200:1 ceiling even though every absolute size is small.
        const std::vector<std::uint8_t> zeros(100000, 0);
        const std::vector<Payload> payloads{{"data/ratio.bin", zeros}};
        auto manifest = manifest_for(payloads);
        const auto manifest_bytes = seal(manifest);
        auto members = members_for(manifest_bytes, payloads);
        members[1].deflate = true;
        const auto refused = admit(sandbox, forge_zip(members));
        CHECK(refused.status == CapsuleStatus::archive_budget_exceeded);
        CHECK(refused.subject == "data/ratio.bin");

        // The control: the same size, deflated, at a ratio near 1:1.
        const std::vector<Payload> honest_payloads{{"data/ratio.bin", noise(100000, 45)}};
        auto honest_manifest = manifest_for(honest_payloads);
        const auto honest_manifest_bytes = seal(honest_manifest);
        auto honest = members_for(honest_manifest_bytes, honest_payloads);
        honest[1].deflate = true;
        CHECK(admit(sandbox, forge_zip(honest)).status == CapsuleStatus::ok);
    }

    SECTION("manifest bytes") {
        const std::vector<Payload> payloads{{"data/a.bin", noise(64, 46)}};
        auto manifest = manifest_for(payloads);
        const auto manifest_bytes = seal(manifest);
        const auto bytes = forge_zip(members_for(manifest_bytes, payloads));

        CapsuleLimits tight = kCapsuleLimitsV1;
        tight.max_manifest_bytes = manifest_bytes.size() - 1;
        const auto refused = admit(sandbox, bytes, tight);
        CHECK(refused.status == CapsuleStatus::archive_budget_exceeded);
        CHECK(refused.subject == std::string(ac::kManifestPath));

        CapsuleLimits exact = kCapsuleLimitsV1;
        exact.max_manifest_bytes = manifest_bytes.size();
        CHECK(admit(sandbox, bytes, exact).status == CapsuleStatus::ok);
    }

    SECTION("expanded bytes from no compressed bytes") {
        // Structurally impossible rather than merely over budget: zero
        // compressed bytes cannot produce any expanded bytes.
        auto members = one_payload_archive("data/impossible.bin");
        members[1].method = 8;
        members[1].raw_payload = std::vector<std::uint8_t>{};
        members[1].declared_compressed = 0;
        members[1].declared_expanded = 4096;
        CHECK(admit(sandbox, forge_zip(members)).status == CapsuleStatus::unsafe_archive);
    }
}

// ── Damaged members ─────────────────────────────────────────────────────

TEST_CASE("a truncated or corrupt member is refused when it is expanded",
          "[authoring-capsule][hostile][member]") {
    Sandbox sandbox;

    const auto content = noise(8192, 51);
    auto compressed = pulp::runtime::deflate_compress(content.data(), content.size(), 6);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() > 32);

    const std::vector<Payload> payloads{{"data/truncated.bin", content}};
    auto manifest = manifest_for(payloads);
    const auto manifest_bytes = seal(manifest);

    SECTION("truncated deflate stream") {
        auto members = members_for(manifest_bytes, payloads);
        auto cut = *compressed;
        cut.resize(cut.size() / 2);
        members[1].method = 8;
        members[1].raw_payload = cut;
        members[1].declared_compressed = static_cast<std::uint32_t>(cut.size());
        members[1].declared_expanded = static_cast<std::uint32_t>(content.size());

        const auto file = sandbox.write("truncated.forged", forge_zip(members));
        auto archive = ac::open_archive(file);
        // The central directory still describes a plausible member; only the
        // bytes are short, so the refusal has to come from expansion.
        REQUIRE(archive);
        auto expanded = archive->read("data/truncated.bin");
        REQUIRE(expanded.is_err());
        CHECK(expanded.error().status == CapsuleStatus::unsafe_archive);
        CHECK(expanded.error().subject == "data/truncated.bin");
    }

    SECTION("member content disagrees with its recorded crc") {
        auto members = members_for(manifest_bytes, payloads);
        members[1].crc = crc32_of(content) ^ 0xFFFFFFFFu;

        const auto file = sandbox.write("badcrc.forged", forge_zip(members));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        auto expanded = archive->read("data/truncated.bin");
        REQUIRE(expanded.is_err());
        CHECK(expanded.error().status == CapsuleStatus::unsafe_archive);
    }

    SECTION("control: the intact member reads back exactly") {
        const auto file = sandbox.write("intact.forged", forge_zip(members_for(manifest_bytes,
                                                                              payloads)));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        auto expanded = archive->read("data/truncated.bin");
        REQUIRE(expanded);
        CHECK((*expanded == content));
    }
}

// ── The manifest member ─────────────────────────────────────────────────

TEST_CASE("the manifest member must be first, bounded, and well formed",
          "[authoring-capsule][hostile][manifest]") {
    Sandbox sandbox;
    const std::vector<Payload> payloads{{"data/a.bin", noise(128, 61)}};
    auto manifest = manifest_for(payloads);
    const auto manifest_bytes = seal(manifest);

    SECTION("manifest not first") {
        auto members = members_for(manifest_bytes, payloads);
        std::swap(members[0], members[1]);
        CHECK(admit(sandbox, forge_zip(members)).status == CapsuleStatus::manifest_not_first);
    }

    SECTION("no manifest at all") {
        std::vector<ForgedMember> members{plain_member("data/a.bin", payloads[0].bytes)};
        CHECK(admit(sandbox, forge_zip(members)).status == CapsuleStatus::manifest_not_first);
    }

    SECTION("manifest under another name") {
        auto members = members_for(manifest_bytes, payloads);
        members[0].name = "manifest.json";
        CHECK(admit(sandbox, forge_zip(members)).status == CapsuleStatus::manifest_not_first);
    }

    SECTION("malformed manifest json") {
        std::vector<ForgedMember> members;
        const std::string broken = "{ \"format\": ";
        members.push_back(plain_member(
            std::string(ac::kManifestPath),
            std::vector<std::uint8_t>(broken.begin(), broken.end())));
        const auto file = sandbox.write("malformed.forged", forge_zip(members));
        auto archive = ac::open_archive(file);
        // The container is fine; only its manifest is not, and that is a
        // manifest verdict rather than an archive one.
        REQUIRE(archive);
        const auto registry = corpus_registry();
        auto preview = ac::preview_capsule(*archive, registry, corpus_options());
        REQUIRE(preview.is_err());
        CHECK(preview.error().status == CapsuleStatus::manifest_invalid);
    }

    SECTION("manifest from another format") {
        auto foreign = manifest;
        foreign.format = "com.example.other-format";
        auto json = ac::to_canonical_json(foreign);
        REQUIRE(json);
        std::vector<ForgedMember> members;
        members.push_back(plain_member(std::string(ac::kManifestPath),
                                       std::vector<std::uint8_t>(json->begin(), json->end())));
        const auto file = sandbox.write("foreign.forged", forge_zip(members));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        const auto registry = corpus_registry();
        auto preview = ac::preview_capsule(*archive, registry, corpus_options());
        REQUIRE(preview.is_err());
        CHECK(preview.error().status == CapsuleStatus::unsupported_format);
    }

    SECTION("manifest from a newer format version") {
        auto newer = manifest;
        newer.format_version = ac::kFormatVersion + 1;
        auto json = ac::to_canonical_json(newer);
        REQUIRE(json);
        std::vector<ForgedMember> members;
        members.push_back(plain_member(std::string(ac::kManifestPath),
                                       std::vector<std::uint8_t>(json->begin(), json->end())));
        const auto file = sandbox.write("newer.forged", forge_zip(members));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        const auto registry = corpus_registry();
        auto preview = ac::preview_capsule(*archive, registry, corpus_options());
        REQUIRE(preview.is_err());
        CHECK(preview.error().status == CapsuleStatus::unsupported_format_version);
    }

    SECTION("format version zero") {
        // Zero is not a version this format ever had, so telling the user to
        // upgrade would be wrong advice.
        auto zero = manifest;
        zero.format_version = 0;
        auto json = ac::to_canonical_json(zero);
        REQUIRE(json);
        std::vector<ForgedMember> members;
        members.push_back(plain_member(std::string(ac::kManifestPath),
                                       std::vector<std::uint8_t>(json->begin(), json->end())));
        const auto file = sandbox.write("zero.forged", forge_zip(members));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        const auto registry = corpus_registry();
        auto preview = ac::preview_capsule(*archive, registry, corpus_options());
        REQUIRE(preview.is_err());
        CHECK(preview.error().status == CapsuleStatus::manifest_invalid);
    }
}

// ── Closure and digests ─────────────────────────────────────────────────

namespace {

/// Open, preview, and report the preview status. Used by the closure family,
/// where the container is always well formed and the lie is in the manifest.
CapsuleStatus preview_status(const Sandbox& sandbox,
                             const std::vector<std::uint8_t>& archive_bytes,
                             std::string* subject = nullptr) {
    const auto file = sandbox.write("closure.forged", archive_bytes);
    const auto before = sandbox.snapshot();
    auto archive = ac::open_archive(file);
    REQUIRE(archive);
    const auto registry = corpus_registry();
    auto preview = ac::preview_capsule(*archive, registry, corpus_options());
    CHECK(sandbox.snapshot() == before);
    if (preview)
        return CapsuleStatus::ok;
    if (subject != nullptr)
        *subject = preview.error().subject;
    return preview.error().status;
}

}  // namespace

TEST_CASE("closure violations are named in both directions",
          "[authoring-capsule][hostile][closure]") {
    Sandbox sandbox;
    const std::vector<Payload> payloads{{"data/a.bin", noise(256, 71)},
                                        {"data/b.bin", noise(256, 72)}};

    SECTION("control: a complete closure previews") {
        CHECK(preview_status(sandbox, well_formed_capsule(payloads)) == CapsuleStatus::ok);
    }

    SECTION("undeclared member present in the archive") {
        auto manifest = manifest_for(payloads);
        const auto manifest_bytes = seal(manifest);
        auto members = members_for(manifest_bytes, payloads);
        members.push_back(plain_member("data/smuggled.bin", noise(128, 73)));
        std::string subject;
        CHECK(preview_status(sandbox, forge_zip(members), &subject) ==
              CapsuleStatus::closure_violation);
        CHECK(subject == "data/smuggled.bin");
    }

    SECTION("declared member absent from the archive") {
        // The row is authored, the digest is recomputed over it, and only the
        // bytes are missing — so the capsule is internally consistent and the
        // reader has to catch the absence itself.
        auto manifest = manifest_for(payloads);
        ac::FileEntry ghost;
        ghost.role = "test.payload";
        ghost.path = "data/ghost.bin";
        ghost.sha256 = digest_of(noise(64, 74));
        ghost.bytes = 64;
        ghost.media_type = "application/octet-stream";
        ghost.policy.source_availability = ac::SourceAvailability::included;
        manifest.files.push_back(std::move(ghost));
        const auto manifest_bytes = seal(manifest);
        std::string subject;
        CHECK(preview_status(sandbox, forge_zip(members_for(manifest_bytes, payloads)),
                             &subject) == CapsuleStatus::closure_violation);
        CHECK(subject == "data/ghost.bin");
    }

    SECTION("two rows for one member") {
        auto manifest = manifest_for(payloads);
        manifest.files.push_back(manifest.files.front());
        const auto manifest_bytes = seal(manifest);
        CHECK(preview_status(sandbox, forge_zip(members_for(manifest_bytes, payloads))) ==
              CapsuleStatus::closure_violation);
    }

    SECTION("a row claiming the manifest itself") {
        // The manifest authors the closure; a row for it would have to carry
        // the digest of the bytes that contain the row.
        auto manifest = manifest_for(payloads);
        ac::FileEntry self;
        self.role = "test.payload";
        self.path = std::string(ac::kManifestPath);
        self.sha256 = digest_of({});
        self.bytes = 0;
        self.media_type = "application/json";
        self.policy.source_availability = ac::SourceAvailability::included;
        manifest.files.push_back(std::move(self));
        const auto manifest_bytes = seal(manifest);
        std::string subject;
        CHECK(preview_status(sandbox, forge_zip(members_for(manifest_bytes, payloads)),
                             &subject) == CapsuleStatus::closure_violation);
        CHECK(subject == std::string(ac::kManifestPath));
    }

    SECTION("a row whose declared size disagrees with its member") {
        auto manifest = manifest_for(payloads);
        manifest.files.front().bytes += 1;
        const auto manifest_bytes = seal(manifest);
        CHECK(preview_status(sandbox, forge_zip(members_for(manifest_bytes, payloads))) ==
              CapsuleStatus::closure_violation);
    }

    SECTION("a row carrying a hostile path") {
        auto manifest = manifest_for(payloads);
        manifest.files.front().path = "../escape.bin";
        const auto manifest_bytes = seal(manifest);
        CHECK(preview_status(sandbox, forge_zip(members_for(manifest_bytes, payloads))) ==
              CapsuleStatus::path_rejected);
    }
}

TEST_CASE("a manifest that lies about its own identity is refused",
          "[authoring-capsule][hostile][closure]") {
    Sandbox sandbox;
    const std::vector<Payload> payloads{{"data/a.bin", noise(256, 81)}};
    auto manifest = manifest_for(payloads);
    auto manifest_bytes = seal(manifest);

    // Everything downstream binds to the digest the reader computed, never to
    // the one the capsule asserts about itself.
    manifest.revision_id = "sha256:" + std::string(64, '0');
    auto json = ac::to_canonical_json(manifest);
    REQUIRE(json);
    manifest_bytes.assign(json->begin(), json->end());

    std::string subject;
    CHECK(preview_status(sandbox, forge_zip(members_for(manifest_bytes, payloads)), &subject) ==
          CapsuleStatus::digest_mismatch);
    CHECK(subject == "revision_id");
}

TEST_CASE("a member whose bytes disagree with its declared digest is never written",
          "[authoring-capsule][hostile][closure]") {
    Sandbox sandbox;
    const std::vector<Payload> payloads{{"data/a.bin", noise(256, 91)},
                                        {"data/b.bin", noise(256, 92)}};

    auto manifest = manifest_for(payloads);
    // Sized correctly, digested wrongly. The size check the preview can make
    // for free therefore passes, and only the digest verified during extraction
    // can catch it — which is the point: the check has to happen before a byte
    // reaches the filesystem.
    manifest.files.front().sha256 = digest_of(noise(256, 999));
    const auto manifest_bytes = seal(manifest);

    const auto file =
        sandbox.write("digest.forged", forge_zip(members_for(manifest_bytes, payloads)));
    auto archive = ac::open_archive(file);
    REQUIRE(archive);
    const auto registry = corpus_registry();
    auto preview = ac::preview_capsule(*archive, registry, corpus_options());
    REQUIRE(preview);

    fs::path staging_root;
    {
        auto staging = ac::StagingArea::create(sandbox.root());
        REQUIRE(staging);
        staging_root = staging->root();
        auto admitted = ac::admit_to_staging(*archive, *preview, registry, *staging);
        REQUIRE(admitted.is_err());
        CHECK(admitted.error().status == CapsuleStatus::digest_mismatch);
        CHECK(admitted.error().subject == "data/a.bin");

        // Refused before the write, so no plausible-looking wrong file was left
        // behind for anything downstream to pick up.
        CHECK_FALSE(fs::exists(staging_root / "data" / "a.bin"));
    }
    // The staging area is private and self-cleaning: a failed import leaves no
    // directory a person could mistake for an imported project.
    CHECK_FALSE(fs::exists(staging_root));

    // Only the capsule file remains in the sandbox.
    CHECK(sandbox.snapshot() == std::vector<std::string>{"file:digest.forged"});
}

// ── Invariants that survive every rejection ─────────────────────────────

TEST_CASE("preview reaches nothing outside the archive it was handed",
          "[authoring-capsule][hostile][preview]") {
    Sandbox sandbox;
    const std::vector<Payload> payloads{{"data/a.bin", noise(256, 101)}};
    const auto file = sandbox.write("capsule.forged", well_formed_capsule(payloads));

    auto archive = ac::open_archive(file);
    REQUIRE(archive);

#ifndef _WIN32
    // Structural, not incidental: `preview_capsule` takes an already-open
    // archive, a registry, and options — it is handed no path at all. Removing
    // the capsule's directory entry while the reader holds it open proves the
    // preview consults no filesystem name, because on POSIX an unlinked file
    // stays readable only through the descriptor that is already open.
    std::error_code ec;
    fs::remove(file, ec);
    REQUIRE_FALSE(ec);
#endif

    const auto before = sandbox.snapshot();
    const auto registry = corpus_registry();
    auto preview = ac::preview_capsule(*archive, registry, corpus_options());
    REQUIRE(preview);
    CHECK(preview->manifest.files.size() == 1);

    // Nothing appeared, nothing changed, nothing was staged.
    CHECK(sandbox.snapshot() == before);
}

TEST_CASE("the capsule substrate contains no outbound or execution call site",
          "[authoring-capsule][hostile][preview]") {
    // Preview promises zero execution and zero network access. That promise is
    // about a code path, so it is checked against the code rather than inferred
    // from one run: a socket opened on a branch this corpus does not reach
    // would still be a network call in the preview path.
    const fs::path source_root(PULP_AUTHORING_CAPSULE_SOURCE_DIR);
    REQUIRE(fs::is_directory(source_root));

    std::vector<std::pair<std::string, std::string>> hits;
    std::size_t control_hits = 0;
    std::size_t scanned = 0;

    // Call sites, not URL text. A scheme literal is data: the module names
    // `https://` precisely in order to REFUSE every other provider form, which
    // is the opposite of reaching out, and flagging it would make the safety
    // check itself the violation. Nothing can be fetched without one of the
    // APIs below, so these are what the promise actually rests on.
    static const std::vector<std::string> kForbidden{
        "socket(",  "connect(",     "getaddrinfo(",  "popen(",      "system(",
        "execve(",  "execvp(",      "execl(",        "fork(",       "dlopen(",
        "dlsym(",   "posix_spawn(", "CreateProcess", "curl_easy_",  "WinHttp",
        "URLSession", "NSURL",      "send(",         "recv(",       "bind(",
    };

    std::error_code ec;
    for (fs::recursive_directory_iterator it(source_root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        REQUIRE_FALSE(ec);
        if (!it->is_regular_file())
            continue;
        const auto extension = it->path().extension().string();
        if (extension != ".cpp" && extension != ".hpp")
            continue;
        std::ifstream in(it->path(), std::ios::binary);
        REQUIRE(in.good());
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        ++scanned;
        // The control token must be found, or the scan below is measuring an
        // empty string and its zero hits mean nothing.
        if (text.find("authoring_capsule") != std::string::npos)
            ++control_hits;
        for (const auto& token : kForbidden) {
            if (text.find(token) != std::string::npos)
                hits.emplace_back(it->path().filename().string(), token);
        }
    }

    INFO("scanned " << scanned << " translation units");
    CHECK(scanned >= 10);
    CHECK(control_hits == scanned);
    for (const auto& [file, token] : hits) {
        INFO("outbound or execution token " << token << " in " << file);
        CHECK(false);
    }
    CHECK(hits.empty());
}

// ── Publication ─────────────────────────────────────────────────────────

TEST_CASE("publication never replaces what is already there",
          "[authoring-capsule][hostile][publication]") {
    Sandbox sandbox;

    SECTION("a staged tree refuses an occupied destination") {
        const std::vector<Payload> payloads{{"data/a.bin", noise(256, 111)}};
        const auto file = sandbox.write("capsule.forged", well_formed_capsule(payloads));
        auto archive = ac::open_archive(file);
        REQUIRE(archive);
        const auto registry = corpus_registry();
        auto preview = ac::preview_capsule(*archive, registry, corpus_options());
        REQUIRE(preview);

        auto staging = ac::StagingArea::create(sandbox.root());
        REQUIRE(staging);
        REQUIRE(ac::admit_to_staging(*archive, *preview, registry, *staging));

        const auto destination = sandbox.root() / "existing-project";
        REQUIRE(fs::create_directory(destination));
        const std::vector<std::uint8_t> occupant{'k', 'e', 'e', 'p'};
        const auto marker = destination / "user-work.txt";
        {
            std::ofstream out(marker, std::ios::binary);
            out.write(reinterpret_cast<const char*>(occupant.data()),
                      static_cast<std::streamsize>(occupant.size()));
        }
        const auto occupied_before = sandbox.snapshot();

        auto published = staging->publish_no_replace(destination);
        REQUIRE(published.is_err());
        CHECK(published.error().status == CapsuleStatus::publication_conflict);

        // The refusal did not touch the thing it refused to replace.
        CHECK(sandbox.snapshot() == occupied_before);
        std::ifstream in(marker, std::ios::binary);
        const std::string kept((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(kept == "keep");

        // Control: a name that is free publishes, so the refusal above is about
        // the occupied destination and not about a staging area that can never
        // publish.
        REQUIRE(staging->publish_no_replace(sandbox.root() / "fresh-project"));
        CHECK(fs::exists(sandbox.root() / "fresh-project" / "data" / "a.bin"));
    }

    SECTION("the writer refuses an occupied destination") {
        const auto destination = sandbox.root() / "already.capsule";
        const std::vector<std::uint8_t> occupant{'o', 'l', 'd'};
        sandbox.write("already.capsule", occupant);
        const auto before = sandbox.snapshot();

        std::vector<ac::WriteMember> members;
        members.push_back(ac::WriteMember{std::string(ac::kManifestPath), noise(64, 112)});
        auto written = ac::write_archive_no_replace(members, destination);
        REQUIRE(written.is_err());
        CHECK(written.error().status == CapsuleStatus::publication_conflict);

        // Unchanged bytes and, just as important, no abandoned temporary file
        // beside the destination for a person to mistake for an export.
        CHECK(sandbox.snapshot() == before);
        std::ifstream in(destination, std::ios::binary);
        const std::string kept((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(kept == "old");

        // Control: the same inventory writes cleanly to a free name.
        CHECK(ac::write_archive_no_replace(members, sandbox.root() / "fresh.capsule"));
    }

    SECTION("a rejected inventory leaves nothing behind") {
        const auto before = sandbox.snapshot();
        std::vector<ac::WriteMember> members;
        members.push_back(ac::WriteMember{std::string(ac::kManifestPath), noise(16, 113)});
        members.push_back(ac::WriteMember{"../escape.bin", noise(16, 114)});
        auto written = ac::write_archive_no_replace(members, sandbox.root() / "out.capsule");
        REQUIRE(written.is_err());
        CHECK(written.error().status == CapsuleStatus::path_rejected);
        CHECK(sandbox.snapshot() == before);
    }
}
