// SPDX-License-Identifier: MIT
//
// importer_install.cpp — install / uninstall mechanism for framework-importer
// add-ons.
//
// An importer is a vendor-specific add-on tool distributed as a checksummed,
// per-platform archive that drives Pulp's JSON-over-stdio import SPI. This file
// is the *install-side contract* that consumes such packages:
//
//   * SDK/SPI version-window enforcement — refuse to install an importer that
//     does not match the running SDK or whose SPI window does not overlap the
//     SDK's, with a loud, user-facing message.
//   * sha256 verification — the fetched/local archive must match the digest the
//     registry pins before anything is unpacked.
//   * skill install — the per-importer SKILL.md is dropped into
//     ~/.agents/skills/<importer>/ on install and removed on uninstall.
//   * install record — id/version/sha/paths recorded under ~/.pulp/importers/
//     so uninstall and version checks work, and so the importer-terms gate can
//     compose with the install (it reads the same ~/.pulp tree).
//
// The producer side (building/hosting/signing the prebuilt artifacts, the
// bundled-libclang choice) is intentionally NOT here — those are user/release
// decisions documented in docs/reference/framework-importer-packaging.md. This
// file only consumes the contract.
//
// Vendor-agnostic: no framework or vendor name appears in this file. Framework
// identity is runtime DATA carried by the registry descriptor.

#include "tool_registry.hpp"
#include "import_spi.hpp"

// The generated version header is only on the include path for the CLI binary
// target (its CMAKE_CURRENT_BINARY_DIR). Unit-test targets that compile this TU
// in isolation drive the SDK version through the PULP_SDK_VERSION env var, so
// the include is optional here.
#if __has_include(<pulp_version_gen.h>)
#  include <pulp_version_gen.h>
#endif

#include <pulp/platform/child_process.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pulp::cli::tools {

namespace {

// ── Console helpers (kept local; tool_registry.cpp's are file-static) ──

std::string c_green(const std::string& s) { return "\033[32m" + s + "\033[0m"; }
std::string c_red(const std::string& s) { return "\033[31m" + s + "\033[0m"; }
std::string c_dim(const std::string& s) { return "\033[2m" + s + "\033[0m"; }

void say_ok(const std::string& m) { std::cout << c_green("✓") << " " << m << "\n"; }
void say_fail(const std::string& m) { std::cerr << c_red("✗") << " " << m << "\n"; }

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool write_text(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << body;
    return f.good();
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// ── Self-contained SHA-256 ──
//
// Hand-rolled so the install mechanism (and its unit tests) need no mbedTLS
// link. FIPS 180-4. Validated against known vectors in the test suite.

struct Sha256 {
    std::array<uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t total_bits = 0;
    std::array<uint8_t, 64> buf{};
    size_t buf_len = 0;

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* data, size_t len) {
        total_bits += uint64_t(len) * 8;
        while (len > 0) {
            size_t take = std::min<size_t>(64 - buf_len, len);
            std::memcpy(buf.data() + buf_len, data, take);
            buf_len += take;
            data += take;
            len -= take;
            if (buf_len == 64) {
                block(buf.data());
                buf_len = 0;
            }
        }
    }

    std::string hex() {
        // Capture the true message length before appending padding.
        const uint64_t bits = total_bits;
        // Append 0x80 then zeros directly into the working buffer (do NOT use
        // update(), which would inflate total_bits — the length word must be
        // the original message length).
        buf[buf_len++] = 0x80;
        if (buf_len > 56) {
            while (buf_len < 64) buf[buf_len++] = 0;
            block(buf.data());
            buf_len = 0;
        }
        while (buf_len < 56) buf[buf_len++] = 0;
        for (int i = 0; i < 8; ++i)
            buf[56 + i] = uint8_t((bits >> ((7 - i) * 8)) & 0xff);
        block(buf.data());
        buf_len = 0;
        static const char* hexd = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t v : h) {
            for (int i = 3; i >= 0; --i) {
                uint8_t byte = uint8_t((v >> (i * 8)) & 0xff);
                out += hexd[byte >> 4];
                out += hexd[byte & 0xf];
            }
        }
        return out;
    }
};

// Strip a leading "file://" scheme so callers can pass either a bare path or a
// file:// URL for the --from override.
std::string strip_file_scheme(const std::string& s) {
    constexpr const char* kPrefix = "file://";
    if (s.rfind(kPrefix, 0) == 0) return s.substr(std::strlen(kPrefix));
    return s;
}

std::string expand_version(const std::string& tmpl, const std::string& version) {
    std::string out = tmpl;
    for (auto pos = out.find("${version}"); pos != std::string::npos;
         pos = out.find("${version}", pos)) {
        out.replace(pos, 10, version);
        pos += version.size();
    }
    return out;
}

}  // namespace

// ── Paths ──

fs::path skills_dir() {
    // Honor PULP_HOME for tests; otherwise the shared agent-skills dir.
    if (const char* env = std::getenv("PULP_HOME"))
        return fs::path(env) / "agents" / "skills";
#ifdef _WIN32
    if (const char* appdata = std::getenv("LOCALAPPDATA"))
        return fs::path(appdata) / "Pulp" / "agents" / "skills";
#else
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".agents" / "skills";
#endif
    return fs::temp_directory_path() / "pulp-agents-skills";
}

fs::path importer_records_dir() { return pulp_home() / "importers"; }

// ── Semver ──

bool parse_semver3(const std::string& s, Semver3& out) {
    out = Semver3{};
    size_t i = 0;
    auto read_num = [&](int& dst) -> bool {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        long v = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            v = v * 10 + (s[i] - '0');
            ++i;
        }
        dst = static_cast<int>(v);
        return true;
    };
    if (!read_num(out.major)) return false;
    if (i < s.size() && s[i] == '.') { ++i; read_num(out.minor); }
    if (i < s.size() && s[i] == '.') { ++i; read_num(out.patch); }
    return true;
}

bool Semver3::operator<(const Semver3& o) const {
    if (major != o.major) return major < o.major;
    if (minor != o.minor) return minor < o.minor;
    return patch < o.patch;
}
bool Semver3::operator<=(const Semver3& o) const { return !(o < *this); }

// ── Compatibility check ──

ImporterCompatResult check_importer_compat(const ToolDescriptor& tool,
                                           const std::string& sdk_version,
                                           int sdk_spi_min,
                                           int sdk_spi_max) {
    ImporterCompatResult r;

    Semver3 sdk;
    if (!parse_semver3(sdk_version, sdk)) {
        r.error = "cannot parse running SDK version '" + sdk_version + "'";
        return r;
    }

    if (!tool.sdk_min.empty()) {
        Semver3 lo;
        if (!parse_semver3(tool.sdk_min, lo)) {
            r.error = "importer '" + tool.id + "' has an unparseable sdk_min '" +
                      tool.sdk_min + "'";
            return r;
        }
        if (sdk < lo) {
            r.error = "importer '" + tool.id + "' requires Pulp SDK >= " + tool.sdk_min +
                      " but this build is " + sdk_version + " (upgrade Pulp)";
            return r;
        }
    }
    if (!tool.sdk_max.empty()) {
        Semver3 hi;
        if (!parse_semver3(tool.sdk_max, hi)) {
            r.error = "importer '" + tool.id + "' has an unparseable sdk_max '" +
                      tool.sdk_max + "'";
            return r;
        }
        if (hi < sdk) {
            r.error = "importer '" + tool.id + "' supports Pulp SDK <= " + tool.sdk_max +
                      " but this build is " + sdk_version +
                      " (upgrade the importer)";
            return r;
        }
    }

    // SPI window overlap: importer [spi_min,spi_max] must intersect the SDK's
    // [sdk_spi_min,sdk_spi_max].
    if (tool.spi_max < sdk_spi_min) {
        r.error = "importer '" + tool.id + "' speaks import-SPI v" +
                  std::to_string(tool.spi_min) + "–v" + std::to_string(tool.spi_max) +
                  " but this Pulp build speaks v" + std::to_string(sdk_spi_min) +
                  "–v" + std::to_string(sdk_spi_max) + " (upgrade the importer)";
        return r;
    }
    if (tool.spi_min > sdk_spi_max) {
        r.error = "importer '" + tool.id + "' speaks import-SPI v" +
                  std::to_string(tool.spi_min) + "–v" + std::to_string(tool.spi_max) +
                  " but this Pulp build speaks v" + std::to_string(sdk_spi_min) +
                  "–v" + std::to_string(sdk_spi_max) + " (upgrade Pulp)";
        return r;
    }

    r.ok = true;
    return r;
}

// ── Checksum ──

std::string sha256_file_hex(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    Sha256 ctx;
    std::array<char, 64 * 1024> chunk{};
    while (f) {
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        std::streamsize got = f.gcount();
        if (got > 0)
            ctx.update(reinterpret_cast<const uint8_t*>(chunk.data()),
                       static_cast<size_t>(got));
    }
    return ctx.hex();
}

// ── Install ──

ImporterInstallResult install_importer(const ToolDescriptor& tool,
                                       const std::string& sdk_version,
                                       int sdk_spi_min,
                                       int sdk_spi_max,
                                       const std::string& from_override,
                                       bool force) {
    ImporterInstallResult result;

    if (tool.category != "importer") {
        result.error = "tool '" + tool.id + "' is not an importer";
        return result;
    }

    // 1. Version-window enforcement (loud failure on mismatch).
    auto compat = check_importer_compat(tool, sdk_version, sdk_spi_min, sdk_spi_max);
    if (!compat.ok) {
        result.error = compat.error;
        return result;
    }

    const auto platform = current_platform_key();
    auto art_it = tool.importer_artifacts.find(platform);
    if (art_it == tool.importer_artifacts.end()) {
        result.error = "importer '" + tool.id + "' has no artifact for " + platform;
        return result;
    }
    const ImporterArtifact& art = art_it->second;

    auto install_dir = tools_dir() / tool.id / tool.pinned_version;
    auto record_path = importer_records_dir() / (tool.id + ".json");

    // Idempotency: an existing record for the pinned version is a no-op.
    // Use --force to re-stage and re-verify the artifact checksum.
    if (!force && fs::exists(record_path) && fs::exists(install_dir)) {
        auto existing = read_text(record_path);
        if (existing.find("\"version\": \"" + tool.pinned_version + "\"") !=
            std::string::npos) {
            result.ok = true;
            result.installed_version = tool.pinned_version;
            result.install_dir = install_dir;
            result.record_path = record_path;
            auto sk = skills_dir() /
                      (tool.skill_name.empty() ? tool.id : tool.skill_name) / "SKILL.md";
            if (fs::exists(sk)) result.skill_path = sk;
            return result;
        }
    }

    // 2. Resolve the archive source: --from override (local/file://) or URL.
    std::string source = from_override.empty()
                             ? expand_version(art.url_template, tool.pinned_version)
                             : from_override;
    bool is_remote = source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;

    auto download_dir = tools_dir() / ".downloads";
    std::error_code ec;
    fs::create_directories(download_dir, ec);
    std::string ext = art.archive_format == "zip"      ? ".zip"
                      : art.archive_format == "tar.xz" ? ".tar.xz"
                                                       : ".tar.gz";
    auto archive_path = download_dir / (tool.id + "-" + tool.pinned_version + ext);

    if (is_remote) {
#ifdef _WIN32
        auto dl = pulp::platform::exec(
            "powershell",
            {"-Command", "Invoke-WebRequest -Uri '" + source + "' -OutFile '" +
                             archive_path.string() + "'"},
            300000);
#else
        auto dl = pulp::platform::exec(
            "curl", {"-sSfL", "-o", archive_path.string(), source}, 300000);
#endif
        if (dl.exit_code != 0) {
            result.error = "download failed: " + source;
            return result;
        }
    } else {
        auto local = fs::path(strip_file_scheme(source));
        if (!fs::exists(local)) {
            result.error = "package not found: " + local.string();
            return result;
        }
        fs::copy_file(local, archive_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.error = "cannot read package: " + local.string();
            return result;
        }
    }

    // 3. sha256 verification — refuse on mismatch.
    if (!art.sha256.empty()) {
        auto actual = sha256_file_hex(archive_path);
        std::string expected = art.sha256;
        // case-insensitive compare
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        if (lower(actual) != lower(expected)) {
            fs::remove(archive_path, ec);
            result.error = "checksum mismatch for importer '" + tool.id +
                           "': expected sha256 " + expected + " but got " + actual +
                           " (refusing to install)";
            return result;
        }
    }

    // 4. Unpack into the managed install dir.
    fs::remove_all(install_dir, ec);
    fs::create_directories(install_dir, ec);
    if (!extract_archive(archive_path, install_dir, art.archive_format)) {
        result.error = "failed to extract package for importer '" + tool.id + "'";
        return result;
    }

    // 5. Install the per-importer SKILL.md, if shipped.
    fs::path installed_skill;
    if (!tool.skill_source.empty()) {
        auto src_skill = install_dir / tool.skill_source;
        if (!fs::exists(src_skill)) {
            result.error = "skill source '" + tool.skill_source +
                           "' not found inside package for importer '" + tool.id + "'";
            return result;
        }
        auto skill_name = tool.skill_name.empty() ? tool.id : tool.skill_name;
        auto dest = skills_dir() / skill_name / "SKILL.md";
        if (!write_text(dest, read_text(src_skill))) {
            result.error = "failed to install skill for importer '" + tool.id + "'";
            return result;
        }
        installed_skill = dest;
    }

    // 6. Record the install under ~/.pulp/importers/ so uninstall + version
    //    checks work and the terms gate can compose against the same tree.
    std::ostringstream rec;
    rec << "{\n"
        << "  \"id\": \"" << json_escape(tool.id) << "\",\n"
        << "  \"version\": \"" << json_escape(tool.pinned_version) << "\",\n"
        << "  \"platform\": \"" << json_escape(platform) << "\",\n"
        << "  \"sha256\": \"" << json_escape(art.sha256) << "\",\n"
        << "  \"sdk_version\": \"" << json_escape(sdk_version) << "\",\n"
        << "  \"spi_min\": " << tool.spi_min << ",\n"
        << "  \"spi_max\": " << tool.spi_max << ",\n"
        << "  \"install_dir\": \"" << json_escape(install_dir.string()) << "\",\n"
        << "  \"skill_path\": \"" << json_escape(installed_skill.string()) << "\",\n"
        << "  \"terms_version\": \"" << json_escape(tool.terms_version) << "\",\n"
        << "  \"terms_vendor_id\": \"" << json_escape(tool.vendor_id) << "\"\n"
        << "}\n";
    if (!write_text(record_path, rec.str())) {
        result.error = "failed to write install record for importer '" + tool.id + "'";
        return result;
    }

    fs::remove(archive_path, ec);

    result.ok = true;
    result.installed_version = tool.pinned_version;
    result.install_dir = install_dir;
    result.skill_path = installed_skill;
    result.record_path = record_path;
    return result;
}

// ── Uninstall ──

bool uninstall_importer(const std::string& importer_id) {
    bool removed = false;
    std::error_code ec;

    auto record_path = importer_records_dir() / (importer_id + ".json");
    std::string skill_name = importer_id;
    if (fs::exists(record_path)) {
        // Recover the skill dir name from the record (defaults to id).
        auto body = read_text(record_path);
        auto pos = body.find("\"skill_path\": \"");
        if (pos != std::string::npos) {
            pos += std::strlen("\"skill_path\": \"");
            auto end = body.find('"', pos);
            if (end != std::string::npos && end > pos) {
                fs::path sp = body.substr(pos, end - pos);
                if (!sp.empty() && sp.has_parent_path())
                    skill_name = sp.parent_path().filename().string();
            }
        }
        fs::remove(record_path, ec);
        removed = true;
    }

    auto install_dir = tools_dir() / importer_id;
    if (fs::exists(install_dir)) {
        fs::remove_all(install_dir, ec);
        removed = true;
    }

    auto skill_dir = skills_dir() / skill_name;
    if (fs::exists(skill_dir)) {
        fs::remove_all(skill_dir, ec);
        removed = true;
    }

    return removed;
}

// ── CLI dispatch (composes with `pulp tool` + `pulp add` alias) ──

namespace {

// The SDK version the running host reports. The real CLI exports this through
// the env var it already sets for subprocesses; tests set PULP_SDK_VERSION to
// drive the version-window checks deterministically. The compiled fallback is
// only used when neither is present.
std::string host_sdk_version() {
    if (const char* v = std::getenv("PULP_SDK_VERSION"); v && *v) return v;
#ifdef PULP_SDK_VERSION_GENERATED
    return PULP_SDK_VERSION_GENERATED;
#else
    return "0.0.0";
#endif
}

}  // namespace

std::optional<int> handle_importer_install(const ToolRegistry& registry,
                                           const std::string& id,
                                           const std::string& from_override,
                                           bool force) {
    auto it = registry.tools.find(id);
    if (it == registry.tools.end() || it->second.category != "importer")
        return std::nullopt;  // not an importer — caller handles generic tools

    const auto& tool = it->second;
    // The SDK speaks exactly one import-SPI version today (kSpiVersion); the
    // window is degenerate [v,v]. When the SDK grows backward compat it widens.
    const int sdk_spi = pulp::cli::import_spi::kSpiVersion;
    auto res = install_importer(tool, host_sdk_version(), sdk_spi, sdk_spi,
                                from_override, force);
    if (!res.ok) {
        say_fail(res.error);
        return 1;
    }
    say_ok("Installed importer " +
           (tool.display_name.empty() ? tool.id : tool.display_name) + " " +
           res.installed_version);
    std::cout << "  " << c_dim(res.install_dir.string()) << "\n";
    if (!res.skill_path.empty())
        std::cout << "  skill: " << c_dim(res.skill_path.string()) << "\n";
    std::cout << "  "
              << c_dim("Run `pulp import` to use it; the importer-terms gate "
                       "applies on first run.")
              << "\n";
    return 0;
}

std::optional<int> handle_importer_uninstall(const ToolRegistry& registry,
                                             const std::string& id) {
    auto it = registry.tools.find(id);
    // Allow uninstall even when the registry no longer lists the importer, as
    // long as an install record exists.
    bool is_importer = it != registry.tools.end() && it->second.category == "importer";
    bool has_record = fs::exists(importer_records_dir() / (id + ".json"));
    if (!is_importer && !has_record) return std::nullopt;

    if (uninstall_importer(id)) {
        say_ok("Uninstalled importer " + id + " (skill removed)");
        return 0;
    }
    say_fail("importer '" + id + "' is not installed");
    return 1;
}

}  // namespace pulp::cli::tools
