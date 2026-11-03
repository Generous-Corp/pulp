#include <pulp/host/graph_identity.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace pulp::host {
namespace {

struct Scalar { std::uint32_t value = 0; std::size_t bytes = 0; };

bool continuation(unsigned char c) { return (c & 0xc0u) == 0x80u; }

bool decode(std::string_view s, std::size_t at, Scalar& out) {
    const auto b = static_cast<unsigned char>(s[at]);
    if (b < 0x80) { out = {b, 1}; return true; }
    if (b >= 0xc2 && b <= 0xdf && at + 1 < s.size() && continuation(s[at + 1])) {
        out = {static_cast<std::uint32_t>(((b & 0x1f) << 6) |
                                          (static_cast<unsigned char>(s[at + 1]) & 0x3f)), 2};
        return true;
    }
    if (b >= 0xe0 && b <= 0xef && at + 2 < s.size() && continuation(s[at + 1]) && continuation(s[at + 2])) {
        const auto b1 = static_cast<unsigned char>(s[at + 1]);
        const auto b2 = static_cast<unsigned char>(s[at + 2]);
        if ((b == 0xe0 && b1 < 0xa0) || (b == 0xed && b1 >= 0xa0)) return false;
        out = {static_cast<std::uint32_t>(((b & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f)), 3};
        return true;
    }
    if (b >= 0xf0 && b <= 0xf4 && at + 3 < s.size() && continuation(s[at + 1]) && continuation(s[at + 2]) && continuation(s[at + 3])) {
        const auto b1 = static_cast<unsigned char>(s[at + 1]);
        const auto b2 = static_cast<unsigned char>(s[at + 2]);
        const auto b3 = static_cast<unsigned char>(s[at + 3]);
        if ((b == 0xf0 && b1 < 0x90) || (b == 0xf4 && b1 >= 0x90)) return false;
        out = {static_cast<std::uint32_t>(((b & 0x07) << 18) | ((b1 & 0x3f) << 12) |
                                          ((b2 & 0x3f) << 6) | (b3 & 0x3f)), 4};
        return true;
    }
    return false;
}

void encode(std::uint32_t cp, std::string& out) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

// Small, deterministic NFC composition table for the combining marks that
// commonly occur in authored labels.  Unknown compositions remain valid UTF-8
// and are compared byte-for-byte; they are never replaced lossy.
std::uint32_t compose(std::uint32_t base, std::uint32_t mark) {
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 21> table{{
        {0x0061, 0x0301}, {0x0065, 0x0301}, {0x0069, 0x0301}, {0x006f, 0x0301}, {0x0075, 0x0301},
        {0x0041, 0x0301}, {0x0045, 0x0301}, {0x0049, 0x0301}, {0x004f, 0x0301}, {0x0055, 0x0301},
        {0x0061, 0x0308}, {0x0065, 0x0308}, {0x006f, 0x0308}, {0x0075, 0x0308},
        {0x0041, 0x0308}, {0x0045, 0x0308}, {0x004f, 0x0308}, {0x0055, 0x0308},
        {0x006e, 0x0303}, {0x0063, 0x0327}, {0x0061, 0x0300},
    }};
    constexpr std::array<std::uint32_t, 21> values{{0x00e1,0x00e9,0x00ed,0x00f3,0x00fa,0x00c1,0x00c9,0x00cd,0x00d3,0x00da,0x00e4,0x00eb,0x00f6,0x00fc,0x00c4,0x00cb,0x00d6,0x00dc,0x00f1,0x00e7,0x00e0}};
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i].first == base && table[i].second == mark) return values[i];
    return 0;
}

bool normalize(std::string_view input, std::string& output, std::string& error) {
    output.clear();
    std::vector<std::uint32_t> cps;
    for (std::size_t i = 0; i < input.size();) {
        Scalar scalar;
        if (!decode(input, i, scalar)) { error = "invalid UTF-8"; return false; }
        if (scalar.value == 0) { error = "embedded NUL"; return false; }
        if (scalar.value < 0x20 || scalar.value == 0x7f) { error = "control character"; return false; }
        cps.push_back(scalar.value); i += scalar.bytes;
    }
    for (std::size_t i = 0; i < cps.size(); ++i) {
        if (i + 1 < cps.size()) {
            if (const auto c = compose(cps[i], cps[i + 1]); c != 0) { encode(c, output); ++i; continue; }
        }
        encode(cps[i], output);
    }
    return true;
}

struct CanonicalSet {
    PortIdentityReport report;
    std::unordered_map<std::string, std::string> aliases;
};

CanonicalSet canonical(std::span<const PortHandle> ports, std::span<const PortIdentityAlias> aliases) {
    CanonicalSet result;
    std::unordered_set<std::string> seen;
    for (const auto& port : ports) {
        std::string name, error;
        if (!normalize(port.name, name, error)) {
            result.report.issues.push_back({error == "control character" ? PortIdentityIssue::Code::ControlCharacter : (error == "embedded NUL" ? PortIdentityIssue::Code::InvalidEncoding : PortIdentityIssue::Code::InvalidEncoding), port.name, error});
            continue;
        }
        if (name.empty() || std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; })) {
            result.report.issues.push_back({PortIdentityIssue::Code::EmptyName, name, "empty or whitespace-only authored name"});
            continue;
        }
        result.report.normalized_names.push_back(name);
    }
    for (const auto& alias : aliases) {
        std::string authored, replacement, error;
        if (!normalize(alias.authored_name, authored, error) || !normalize(alias.replacement_name, replacement, error) || alias.contract_version == 0 || authored == replacement) {
            result.report.issues.push_back({PortIdentityIssue::Code::InvalidAlias, alias.authored_name, "alias must have distinct valid names and a contract version"});
            continue;
        }
        if (result.aliases.contains(replacement) || result.aliases.contains(authored)) {
            result.report.issues.push_back({PortIdentityIssue::Code::AliasCollision, replacement, "alias is ambiguous"});
            continue;
        }
        result.aliases.emplace(replacement, authored);
    }
    for (auto& name : result.report.normalized_names) {
        if (const auto it = result.aliases.find(name); it != result.aliases.end()) name = it->second;
        if (!seen.insert(name).second)
            result.report.issues.push_back({PortIdentityIssue::Code::DuplicateName, name, "normalized authored name collides"});
    }
    result.report.valid = result.report.issues.empty();
    return result;
}

void hash_byte(std::uint64_t& h, std::uint8_t b) { h ^= b; h *= 1099511628211ull; }
void hash_string(std::uint64_t& h, std::string_view value) {
    for (const auto c : value) hash_byte(h, static_cast<std::uint8_t>(c));
    hash_byte(h, 0);
}

} // namespace

PortIdentityReport validate_port_identities(std::span<const PortHandle> ports,
                                            std::span<const PortIdentityAlias> aliases) {
    return canonical(ports, aliases).report;
}

std::uint64_t port_identity_fingerprint(std::span<const PortHandle> ports,
                                         std::span<const PortIdentityAlias> aliases) {
    auto set = canonical(ports, aliases);
    if (!set.report.valid) return 0;
    std::sort(set.report.normalized_names.begin(), set.report.normalized_names.end());
    std::uint64_t hash = 1469598103934665603ull;
    hash_string(hash, "pulp.g3.port-identity.v1");
    for (const auto& name : set.report.normalized_names) hash_string(hash, name);
    return hash;
}

PortIdentityDiff diff_port_identities(std::span<const PortHandle> before,
                                      std::span<const PortHandle> after,
                                      std::span<const PortIdentityAlias> aliases) {
    const auto lhs = canonical(before, aliases), rhs = canonical(after, aliases);
    PortIdentityDiff diff;
    diff.valid = lhs.report.valid && rhs.report.valid;
    if (!diff.valid) return diff;
    auto left = lhs.report.normalized_names, right = rhs.report.normalized_names;
    for (const auto& name : right) if (std::find(left.begin(), left.end(), name) == left.end()) diff.added.push_back(name);
    for (const auto& name : left) if (std::find(right.begin(), right.end(), name) == right.end()) diff.removed.push_back(name);
    diff.order_changed = diff.added.empty() && diff.removed.empty() && left != right;
    // A single set replacement with stable order is a rename candidate;
    // callers still require a declared alias before applying it.
    if (!diff.order_changed && diff.added.size() == 1 && diff.removed.size() == 1)
        diff.renamed.push_back(diff.removed.front() + " -> " + diff.added.front());
    return diff;
}

PortIdentityUndo capture_port_identity_undo(std::span<const PortHandle> before,
                                            std::span<const PortIdentityAlias> aliases) {
    PortIdentityUndo undo;
    undo.valid = validate_port_identities(before, aliases).valid;
    if (undo.valid) undo.before.assign(before.begin(), before.end());
    return undo;
}

std::vector<PortHandle> undo_port_identity(const PortIdentityUndo& undo) {
    return undo.valid ? undo.before : std::vector<PortHandle>{};
}

} // namespace pulp::host
