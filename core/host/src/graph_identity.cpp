#include <pulp/host/graph_identity.hpp>

#include "unicode_nfc_data.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {

struct Scalar {
    std::uint32_t value = 0;
    std::size_t bytes = 0;
};

bool continuation(unsigned char c) {
    return (c & 0xc0u) == 0x80u;
}

bool decode(std::string_view s, std::size_t at, Scalar& out) {
    const auto b = static_cast<unsigned char>(s[at]);
    if (b < 0x80) {
        out = {b, 1};
        return true;
    }
    if (b >= 0xc2 && b <= 0xdf && at + 1 < s.size() && continuation(s[at + 1])) {
        out = {static_cast<std::uint32_t>(((b & 0x1f) << 6) |
                                          (static_cast<unsigned char>(s[at + 1]) & 0x3f)),
               2};
        return true;
    }
    if (b >= 0xe0 && b <= 0xef && at + 2 < s.size() && continuation(s[at + 1]) &&
        continuation(s[at + 2])) {
        const auto b1 = static_cast<unsigned char>(s[at + 1]);
        const auto b2 = static_cast<unsigned char>(s[at + 2]);
        if ((b == 0xe0 && b1 < 0xa0) || (b == 0xed && b1 >= 0xa0))
            return false;
        out = {static_cast<std::uint32_t>(((b & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f)),
               3};
        return true;
    }
    if (b >= 0xf0 && b <= 0xf4 && at + 3 < s.size() && continuation(s[at + 1]) &&
        continuation(s[at + 2]) && continuation(s[at + 3])) {
        const auto b1 = static_cast<unsigned char>(s[at + 1]);
        const auto b2 = static_cast<unsigned char>(s[at + 2]);
        const auto b3 = static_cast<unsigned char>(s[at + 3]);
        if ((b == 0xf0 && b1 < 0x90) || (b == 0xf4 && b1 >= 0x90))
            return false;
        out = {static_cast<std::uint32_t>(((b & 0x07) << 18) | ((b1 & 0x3f) << 12) |
                                          ((b2 & 0x3f) << 6) | (b3 & 0x3f)),
               4};
        return true;
    }
    return false;
}

void encode(std::uint32_t cp, std::string& out) {
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
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

std::uint8_t combining_class(std::uint32_t code_point) {
    const auto* first = std::begin(unicode_nfc::kCombiningClasses);
    const auto* last = std::end(unicode_nfc::kCombiningClasses);
    const auto it = std::lower_bound(first, last, code_point,
                                     [](const unicode_nfc::CombiningClass& entry,
                                        std::uint32_t value) { return entry.code_point < value; });
    return it != last && it->code_point == code_point ? it->value : 0;
}

bool control_character(std::uint32_t code_point) {
    return code_point <= 0x001fu || (code_point >= 0x007fu && code_point <= 0x009fu);
}

bool whitespace_character(std::uint32_t code_point) {
    return (code_point >= 0x0009u && code_point <= 0x000du) || code_point == 0x0020u ||
           code_point == 0x0085u || code_point == 0x00a0u || code_point == 0x1680u ||
           (code_point >= 0x2000u && code_point <= 0x200au) || code_point == 0x2028u ||
           code_point == 0x2029u || code_point == 0x202fu || code_point == 0x205fu ||
           code_point == 0x3000u;
}

constexpr std::uint32_t kHangulSBase = 0xac00u;
constexpr std::uint32_t kHangulLBase = 0x1100u;
constexpr std::uint32_t kHangulVBase = 0x1161u;
constexpr std::uint32_t kHangulTBase = 0x11a7u;
constexpr std::uint32_t kHangulLCount = 19u;
constexpr std::uint32_t kHangulVCount = 21u;
constexpr std::uint32_t kHangulTCount = 28u;
constexpr std::uint32_t kHangulNCount = kHangulVCount * kHangulTCount;
constexpr std::uint32_t kHangulSCount = kHangulLCount * kHangulNCount;

void decompose(std::uint32_t code_point, std::vector<std::uint32_t>& output) {
    if (code_point >= kHangulSBase && code_point < kHangulSBase + kHangulSCount) {
        const auto index = code_point - kHangulSBase;
        output.push_back(kHangulLBase + index / kHangulNCount);
        output.push_back(kHangulVBase + (index % kHangulNCount) / kHangulTCount);
        if (index % kHangulTCount != 0)
            output.push_back(kHangulTBase + index % kHangulTCount);
        return;
    }

    const auto* first = std::begin(unicode_nfc::kDecompositions);
    const auto* last = std::end(unicode_nfc::kDecompositions);
    const auto it = std::lower_bound(first, last, code_point,
                                     [](const unicode_nfc::Decomposition& entry,
                                        std::uint32_t value) { return entry.code_point < value; });
    if (it == last || it->code_point != code_point) {
        output.push_back(code_point);
        return;
    }
    decompose(it->first, output);
    if (it->has_second)
        decompose(it->second, output);
}

std::uint32_t compose(std::uint32_t first, std::uint32_t second) {
    if (first >= kHangulLBase && first < kHangulLBase + kHangulLCount && second >= kHangulVBase &&
        second < kHangulVBase + kHangulVCount) {
        return kHangulSBase + (first - kHangulLBase) * kHangulNCount +
               (second - kHangulVBase) * kHangulTCount;
    }
    if (first >= kHangulSBase && first < kHangulSBase + kHangulSCount &&
        (first - kHangulSBase) % kHangulTCount == 0 && second > kHangulTBase &&
        second <= kHangulTBase + kHangulTCount - 1) {
        return first + second - kHangulTBase;
    }

    const auto* first_entry = std::begin(unicode_nfc::kCompositions);
    const auto* last_entry = std::end(unicode_nfc::kCompositions);
    const auto it = std::lower_bound(first_entry, last_entry, std::pair{first, second},
                                     [](const unicode_nfc::Composition& entry,
                                        const std::pair<std::uint32_t, std::uint32_t>& value) {
                                         return std::pair{entry.first, entry.second} < value;
                                     });
    return it != last_entry && it->first == first && it->second == second ? it->composite : 0;
}

bool normalize(std::string_view input, std::string& output, std::string& error) {
    std::vector<std::uint32_t> input_code_points;
    for (std::size_t i = 0; i < input.size();) {
        Scalar scalar;
        if (!decode(input, i, scalar)) {
            error = "invalid UTF-8";
            return false;
        }
        if (scalar.value == 0) {
            error = "embedded NUL";
            return false;
        }
        if (control_character(scalar.value)) {
            error = "control character";
            return false;
        }
        input_code_points.push_back(scalar.value);
        i += scalar.bytes;
    }

    std::vector<std::uint32_t> decomposed;
    for (const auto code_point : input_code_points)
        decompose(code_point, decomposed);

    for (std::size_t segment = 0; segment < decomposed.size();) {
        std::size_t end = segment + 1;
        while (end < decomposed.size() && combining_class(decomposed[end]) != 0)
            ++end;
        std::stable_sort(decomposed.begin() + static_cast<std::ptrdiff_t>(segment),
                         decomposed.begin() + static_cast<std::ptrdiff_t>(end),
                         [](std::uint32_t lhs, std::uint32_t rhs) {
                             return combining_class(lhs) < combining_class(rhs);
                         });
        segment = end;
    }

    std::vector<std::uint32_t> composed;
    std::size_t starter = static_cast<std::size_t>(-1);
    std::uint8_t last_class = 0;
    for (const auto code_point : decomposed) {
        const auto code_point_class = combining_class(code_point);
        if (code_point_class == 0) {
            starter = composed.size();
            last_class = 0;
            composed.push_back(code_point);
            continue;
        }
        if (starter != static_cast<std::size_t>(-1) && last_class < code_point_class) {
            if (const auto replacement = compose(composed[starter], code_point); replacement != 0) {
                composed[starter] = replacement;
                continue;
            }
        }
        composed.push_back(code_point);
        last_class = code_point_class;
    }

    output.clear();
    for (const auto code_point : composed)
        encode(code_point, output);
    return true;
}

bool whitespace_only(std::string_view input) {
    bool saw_code_point = false;
    for (std::size_t i = 0; i < input.size();) {
        Scalar scalar;
        if (!decode(input, i, scalar))
            return false;
        saw_code_point = true;
        if (!whitespace_character(scalar.value))
            return false;
        i += scalar.bytes;
    }
    return saw_code_point;
}

struct CanonicalSet {
    PortIdentityReport report;
    std::unordered_map<std::string, std::string> aliases;
};

CanonicalSet canonical(std::span<const PortHandle> ports,
                       std::span<const PortIdentityAlias> aliases) {
    CanonicalSet result;
    std::unordered_set<std::string> seen;
    for (const auto& port : ports) {
        std::string name, error;
        if (!normalize(port.name, name, error)) {
            result.report.issues.push_back(
                {error == "control character"
                     ? PortIdentityIssue::Code::ControlCharacter
                     : (error == "embedded NUL" ? PortIdentityIssue::Code::InvalidEncoding
                                                : PortIdentityIssue::Code::InvalidEncoding),
                 port.name, error});
            continue;
        }
        if (name.empty() || whitespace_only(name)) {
            result.report.issues.push_back({PortIdentityIssue::Code::EmptyName, name,
                                            "empty or whitespace-only authored name"});
            continue;
        }
        result.report.normalized_names.push_back(name);
    }
    for (const auto& alias : aliases) {
        std::string authored, replacement, error;
        if (!normalize(alias.authored_name, authored, error) ||
            !normalize(alias.replacement_name, replacement, error) || alias.contract_version == 0 ||
            authored == replacement) {
            result.report.issues.push_back(
                {PortIdentityIssue::Code::InvalidAlias, alias.authored_name,
                 "alias must have distinct valid names and a contract version"});
            continue;
        }
        if (result.aliases.contains(replacement) || result.aliases.contains(authored)) {
            result.report.issues.push_back(
                {PortIdentityIssue::Code::AliasCollision, replacement, "alias is ambiguous"});
            continue;
        }
        result.aliases.emplace(replacement, authored);
    }
    for (auto& name : result.report.normalized_names) {
        if (const auto it = result.aliases.find(name); it != result.aliases.end())
            name = it->second;
        if (!seen.insert(name).second)
            result.report.issues.push_back({PortIdentityIssue::Code::DuplicateName, name,
                                            "normalized authored name collides"});
    }
    result.report.valid = result.report.issues.empty();
    return result;
}

void hash_byte(std::uint64_t& h, std::uint8_t b) {
    h ^= b;
    h *= 1099511628211ull;
}
void hash_string(std::uint64_t& h, std::string_view value) {
    for (const auto c : value)
        hash_byte(h, static_cast<std::uint8_t>(c));
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
    if (!set.report.valid)
        return 0;
    std::sort(set.report.normalized_names.begin(), set.report.normalized_names.end());
    std::uint64_t hash = 1469598103934665603ull;
    hash_string(hash, "pulp.g3.port-identity.v1");
    for (const auto& name : set.report.normalized_names)
        hash_string(hash, name);
    return hash;
}

PortIdentityDiff diff_port_identities(std::span<const PortHandle> before,
                                      std::span<const PortHandle> after,
                                      std::span<const PortIdentityAlias> aliases) {
    const auto lhs = canonical(before, aliases), rhs = canonical(after, aliases);
    PortIdentityDiff diff;
    diff.valid = lhs.report.valid && rhs.report.valid;
    if (!diff.valid)
        return diff;
    auto left = lhs.report.normalized_names, right = rhs.report.normalized_names;
    for (const auto& name : right)
        if (std::find(left.begin(), left.end(), name) == left.end())
            diff.added.push_back(name);
    for (const auto& name : left)
        if (std::find(right.begin(), right.end(), name) == right.end())
            diff.removed.push_back(name);
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
    if (undo.valid)
        undo.before.assign(before.begin(), before.end());
    return undo;
}

std::vector<PortHandle> undo_port_identity(const PortIdentityUndo& undo) {
    return undo.valid ? undo.before : std::vector<PortHandle>{};
}

} // namespace pulp::host
