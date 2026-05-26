#include <pulp/ship/appcast.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/base64.hpp>
#include <sstream>
#include <cstring>
#include <regex>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace pulp::ship {

// ── Appcast XML generation ───────────────────────────────────────────────────

static std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string Appcast::to_xml() const {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml << "<rss version=\"2.0\" xmlns:sparkle=\"http://www.andymatuschak.org/xml-namespaces/sparkle\">\n";
    xml << "  <channel>\n";
    xml << "    <title>" << xml_escape(title) << "</title>\n";
    xml << "    <link>" << xml_escape(link) << "</link>\n";
    xml << "    <description>" << xml_escape(description) << "</description>\n";
    xml << "    <language>en</language>\n";

    for (auto& item : items) {
        xml << "    <item>\n";
        xml << "      <title>" << xml_escape(item.title) << "</title>\n";
        if (!item.description.empty()) {
            xml << "      <description><![CDATA[" << item.description << "]]></description>\n";
        }
        xml << "      <pubDate>" << item.pub_date << "</pubDate>\n";
        xml << "      <sparkle:version>" << xml_escape(item.build_number.empty() ? item.version : item.build_number) << "</sparkle:version>\n";
        xml << "      <sparkle:shortVersionString>" << xml_escape(item.version) << "</sparkle:shortVersionString>\n";
        if (!item.minimum_os.empty()) {
            xml << "      <sparkle:minimumSystemVersion>" << xml_escape(item.minimum_os) << "</sparkle:minimumSystemVersion>\n";
        }
        xml << "      <enclosure\n";
        xml << "        url=\"" << xml_escape(item.download_url) << "\"\n";
        xml << "        length=\"" << item.file_size << "\"\n";
        xml << "        type=\"application/octet-stream\"\n";
        if (!item.ed_signature.empty()) {
            xml << "        sparkle:edSignature=\"" << item.ed_signature << "\"\n";
        }
        xml << "      />\n";
        xml << "    </item>\n";
    }

    xml << "  </channel>\n";
    xml << "</rss>\n";
    return xml.str();
}

// ── Simple appcast XML parsing ───────────────────────────────────────────────

static std::string extract_tag(const std::string& xml, const std::string& tag) {
    auto open = "<" + tag + ">";
    auto close = "</" + tag + ">";
    auto start = xml.find(open);
    if (start == std::string::npos) return {};
    start += open.size();
    auto end = xml.find(close, start);
    if (end == std::string::npos) return {};
    return xml.substr(start, end - start);
}

static std::string extract_attr(const std::string& xml, const std::string& attr) {
    auto pos = xml.find(attr + "=\"");
    if (pos == std::string::npos) return {};
    pos += attr.size() + 2;
    auto end = xml.find('"', pos);
    if (end == std::string::npos) return {};
    return xml.substr(pos, end - pos);
}

std::optional<Appcast> Appcast::from_xml(const std::string& xml) {
    if (xml.find("<rss") == std::string::npos) return std::nullopt;

    Appcast feed;
    feed.title = extract_tag(xml, "title");
    feed.link = extract_tag(xml, "link");
    feed.description = extract_tag(xml, "description");

    // Find all <item>...</item> blocks
    size_t pos = 0;
    while (true) {
        auto item_start = xml.find("<item>", pos);
        if (item_start == std::string::npos) break;
        auto item_end = xml.find("</item>", item_start);
        if (item_end == std::string::npos) break;

        auto item_xml = xml.substr(item_start, item_end - item_start + 7);

        AppcastItem item;
        item.title = extract_tag(item_xml, "title");
        item.pub_date = extract_tag(item_xml, "pubDate");
        item.version = extract_tag(item_xml, "sparkle:shortVersionString");
        item.build_number = extract_tag(item_xml, "sparkle:version");
        item.minimum_os = extract_tag(item_xml, "sparkle:minimumSystemVersion");
        item.download_url = extract_attr(item_xml, "url");
        item.ed_signature = extract_attr(item_xml, "sparkle:edSignature");

        auto length_str = extract_attr(item_xml, "length");
        if (!length_str.empty()) {
            try {
                size_t parsed = 0;
                auto size = std::stoull(length_str, &parsed);
                if (parsed == length_str.size())
                    item.file_size = size;
            } catch (...) {
                item.file_size = 0;
            }
        }

        // Extract CDATA description
        auto cdata_start = item_xml.find("<![CDATA[");
        if (cdata_start != std::string::npos) {
            cdata_start += 9;
            auto cdata_end = item_xml.find("]]>", cdata_start);
            if (cdata_end != std::string::npos)
                item.description = item_xml.substr(cdata_start, cdata_end - cdata_start);
        }

        feed.items.push_back(std::move(item));
        pos = item_end + 7;
    }

    return feed;
}

// ── Version comparison ───────────────────────────────────────────────────────

int compare_versions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::istringstream ss(v);
        std::string part;
        while (std::getline(ss, part, '.')) {
            try { parts.push_back(std::stoi(part)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };

    auto pa = parse(a), pb = parse(b);
    size_t len = std::max(pa.size(), pb.size());
    pa.resize(len, 0);
    pb.resize(len, 0);

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

// ── EdDSA signing (Sparkle) ──────────────────────────────────────────────────
//
// Sparkle's `sign_update` Python helper accepts a 32-byte Ed25519 seed
// (base64) OR a 64-byte NaCl-form secret key (seed || public key,
// base64). We accept both. Output is a base64-encoded 64-byte
// detached signature, which is the literal value written into the
// `sparkle:edSignature` attribute on `<enclosure>`.
//
// Backed by `pulp::runtime::ed25519_*` (TweetNaCl, RFC 8032).
//
// Returns std::nullopt on:
//   - unreadable file
//   - private key that decodes to neither 32 nor 64 bytes
//   - any signing-call failure
// Callers MUST treat nullopt as a hard failure (#295). Earlier
// behaviour wrote an empty `edSignature=""` into the appcast and
// logged success — Sparkle then parsed the unsigned release as
// "not signed yet" while operators believed they had shipped a
// signed update. Tracking issue is the same #295.

namespace {

std::optional<std::vector<uint8_t>> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    return bytes;
}

} // namespace

std::optional<std::string> sign_file_ed25519(const std::string& file_path,
                                             const std::string& private_key_b64) {
    auto file_bytes = read_file_bytes(file_path);
    if (!file_bytes) return std::nullopt;

    auto key_bytes = pulp::runtime::base64_decode(private_key_b64);
    if (!key_bytes) return std::nullopt;

    // Sparkle's generate_keys emits a 32-byte seed (modern) but older
    // versions store the full 64-byte NaCl secret key (seed || pk).
    // Accept either; for the seed form, derive the matching keypair via
    // pulp::runtime::ed25519_keypair_from_seed().
    std::vector<uint8_t> sk;
    if (key_bytes->size() == pulp::runtime::ed25519_seed_size) {
        auto kp = pulp::runtime::ed25519_keypair_from_seed(
            key_bytes->data(), key_bytes->size());
        if (!kp) return std::nullopt;
        sk = std::move(kp->private_key);
    } else if (key_bytes->size() == pulp::runtime::ed25519_private_key_size) {
        sk = std::move(*key_bytes);
    } else {
        return std::nullopt;
    }

    auto sig = pulp::runtime::ed25519_sign(
        sk.data(), sk.size(),
        file_bytes->data(), file_bytes->size());
    if (!sig) return std::nullopt;

    return pulp::runtime::base64_encode(sig->data(), sig->size());
}

// ── Sparkle Ed25519 verification ─────────────────────────────────────────────
//
// Verify a base64-encoded 64-byte detached Ed25519 signature against a
// file's raw bytes and a base64-encoded 32-byte public key. Mirrors what
// Sparkle does internally on the host. Useful for:
//   1. Local round-trip tests (sign -> verify before publishing).
//   2. Self-update flows that re-verify a downloaded artifact before
//      install, even if Sparkle has already done so.
bool verify_file_ed25519(const std::string& file_path,
                         const std::string& signature_b64,
                         const std::string& public_key_b64) {
    auto file_bytes = read_file_bytes(file_path);
    if (!file_bytes) return false;

    auto pk = pulp::runtime::base64_decode(public_key_b64);
    if (!pk || pk->size() != pulp::runtime::ed25519_public_key_size) return false;

    auto sig = pulp::runtime::base64_decode(signature_b64);
    if (!sig || sig->size() != pulp::runtime::ed25519_signature_size) return false;

    return pulp::runtime::ed25519_verify(
        pk->data(), pk->size(),
        sig->data(), sig->size(),
        file_bytes->data(), file_bytes->size());
}

// Verify every `<enclosure sparkle:edSignature="...">` item in an appcast
// against the supplied base64 public key. Each item's download_url is
// expected to be a local file path (so the caller has already downloaded
// the artifact) — for HTTP URLs the caller must fetch + hand a local
// path. Returns true iff every signed item verifies and at least one
// item carried a signature. Items without `ed_signature` are skipped.
bool verify_appcast_signatures(const Appcast& feed,
                               const std::string& public_key_b64) {
    bool saw_signature = false;
    for (auto& item : feed.items) {
        if (item.ed_signature.empty()) continue;
        saw_signature = true;
        if (!verify_file_ed25519(item.download_url, item.ed_signature,
                                 public_key_b64))
            return false;
    }
    return saw_signature;
}

} // namespace pulp::ship
