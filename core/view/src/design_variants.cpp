// design_variants.cpp — variant sets -> typed parameterized component contract.

#include <pulp/design/design_variants.hpp>

#include <choc/text/choc_JSON.h>
#include <choc/text/choc_StringUtilities.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace pulp::design {

namespace {

// Is this enum boolean-shaped, and if so what is its falsey value? Returns the
// falsey value ("Off"/"False"/"No") or "" when the enum is not boolean-shaped.
std::string boolean_falsey(const std::vector<std::string>& values) {
    if (values.size() != 2) return {};
    std::string a = choc::text::toLowerCase(values[0]), b = choc::text::toLowerCase(values[1]);
    auto pair_is = [&](const char* t, const char* f) {
        return (a == t && b == f) || (a == f && b == t);
    };
    for (auto [t, f] : {std::pair{"on", "off"}, std::pair{"true", "false"},
                        std::pair{"yes", "no"}, std::pair{"enabled", "disabled"}}) {
        if (pair_is(t, f)) return choc::text::toLowerCase(values[0]) == f ? values[0] : values[1];
    }
    return {};
}

}  // namespace

namespace {

// Split a variant name into trimmed, non-empty comma segments.
std::vector<std::string_view> variant_segments(std::string_view name) {
    std::vector<std::string_view> segs;
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == ',') {
            std::string_view seg = choc::text::trim(name.substr(start, i - start));
            start = i + 1;
            if (!seg.empty()) segs.push_back(seg);
        }
    }
    return segs;
}

}  // namespace

std::vector<VariantProperty> parse_variant_name(std::string_view name) {
    std::vector<VariantProperty> props;
    for (std::string_view seg : variant_segments(name)) {
        auto eq = seg.find('=');
        if (eq == std::string_view::npos) continue;  // malformed segment: caller flags it
        props.push_back(
            {std::string(choc::text::trim(seg.substr(0, eq))), std::string(choc::text::trim(seg.substr(eq + 1)))});
    }
    return props;
}

ComponentContract build_component_contract(std::string_view component,
                                           const std::vector<std::string>& variant_names) {
    ComponentContract c;
    c.format_version = std::string(kVariantContractVersion);
    c.component = std::string(component);

    // Per-property value -> count (across variants), and per-variant key set.
    std::map<std::string, std::map<std::string, int>> value_counts;  // prop -> value -> n
    std::map<std::string, int> key_set_freq;                          // "a|b|c" -> n
    std::vector<std::pair<std::string, std::string>> malformed;       // (variant, segment)
    std::vector<std::pair<std::string, std::vector<std::string>>> per_variant_keys;
    std::vector<std::string> duplicate_variants;                      // repeated whole variants
    std::set<std::string> seen_variants;  // canonical form -> already counted
    int distinct_count = 0;

    for (const auto& vn : variant_names) {
        // A whole variant repeated (copy-paste, or reordered segments) must be
        // counted once — otherwise a duplicate line skews the modal value/key set
        // and can flip the chosen default. Canonicalize on the sorted trimmed
        // segments so "A=1, B=2" and "B=2, A=1" collapse to the same variant.
        std::vector<std::string> canon_segs;
        for (std::string_view seg : variant_segments(vn)) canon_segs.emplace_back(seg);
        std::sort(canon_segs.begin(), canon_segs.end());
        std::string canon;
        for (const auto& s : canon_segs) canon += s + "\n";
        if (!canon.empty() && !seen_variants.insert(canon).second) {
            duplicate_variants.push_back(vn);
            continue;  // do not let the repeat inflate any count
        }
        ++distinct_count;

        std::vector<std::string> keys;
        std::set<std::string> seen_keys;  // dedupe keys within this one variant
        bool any_valid = false;
        for (std::string_view seg : variant_segments(vn)) {
            auto eq = seg.find('=');
            if (eq == std::string_view::npos) {
                malformed.emplace_back(vn, std::string(seg));  // segment with no Prop=Value
                continue;
            }
            std::string key(choc::text::trim(seg.substr(0, eq)));
            std::string val(choc::text::trim(seg.substr(eq + 1)));
            if (key.empty() || val.empty()) {  // "=Primary" / "State=" — not a usable assignment
                malformed.emplace_back(vn, std::string(seg));
                continue;
            }
            if (!seen_keys.insert(key).second) {  // "State=On, State=Off" — ambiguous within a variant
                malformed.emplace_back(vn, "duplicate property \"" + key + "\"");
                continue;  // count only the first occurrence
            }
            value_counts[key][val]++;
            keys.push_back(key);
            any_valid = true;
        }
        std::sort(keys.begin(), keys.end());
        std::string joined;
        for (size_t i = 0; i < keys.size(); ++i) joined += (i ? "|" : "") + keys[i];
        // Only variants with at least one valid property inform the modal key
        // set; a wholly-malformed variant is already flagged and must not skew it.
        if (any_valid) {
            key_set_freq[joined]++;
            per_variant_keys.emplace_back(vn, std::move(keys));
        }
    }

    // Build a ContractProp per property, sorted by property name (map order).
    for (auto& [prop, counts] : value_counts) {
        ContractProp cp;
        cp.name = prop;
        for (auto& [val, n] : counts) cp.values.push_back(val);  // map keys already sorted+unique
        // Default: explicit "default" value, else boolean falsey, else modal.
        std::string def;
        for (const auto& v : cp.values)
            if (choc::text::toLowerCase(v) == "default") { def = v; break; }
        if (def.empty()) def = boolean_falsey(cp.values);
        if (def.empty()) {
            int best = -1;
            for (const auto& v : cp.values) {  // cp.values is sorted -> deterministic ties
                int n = counts[v];
                if (n > best) { best = n; def = v; }
            }
        }
        cp.default_value = def;
        c.props.push_back(std::move(cp));
    }

    // Count distinct variants, not raw list entries — a repeated line is not a
    // second variant.
    c.variant_count = distinct_count;

    // Issues, in a stable order.
    // "Single-variant" is about how many variants actually *inform* the contract:
    // a set of {malformed, one-real} is effectively a single-variant set even
    // though the raw list has two entries.
    if (per_variant_keys.size() < 2)
        c.issues.push_back({"single-variant", "component set has " +
                                                  std::to_string(per_variant_keys.size()) +
                                                  " informative variant(s)"});
    for (const auto& vn : duplicate_variants)
        c.issues.push_back(
            {"duplicate-variant", "variant \"" + vn + "\" is a repeat and was counted once"});
    for (auto& [vn, seg] : malformed)
        c.issues.push_back({"malformed-name", "variant \"" + vn + "\" segment \"" + seg +
                                                  "\" has no Prop=Value"});
    // The modal key set is the most common; any variant that differs is flagged.
    if (!key_set_freq.empty()) {
        std::string modal;
        int best = -1;
        for (auto& [ks, n] : key_set_freq)
            if (n > best) { best = n; modal = ks; }
        for (auto& [vn, keys] : per_variant_keys) {
            std::string joined;
            for (size_t i = 0; i < keys.size(); ++i) joined += (i ? "|" : "") + keys[i];
            if (joined != modal)
                c.issues.push_back({"inconsistent-props",
                                    "variant \"" + vn + "\" props [" + joined +
                                        "] differ from the set's [" + modal + "]"});
        }
    }
    return c;
}

std::string component_contract_json(const ComponentContract& contract) {
    auto root = choc::value::createObject("");
    root.addMember("format_version", contract.format_version);
    root.addMember("component", contract.component);
    root.addMember("variant_count", static_cast<int64_t>(contract.variant_count));

    auto props = choc::value::createEmptyArray();
    for (const auto& p : contract.props) {
        auto po = choc::value::createObject("");
        po.addMember("name", p.name);
        po.addMember("default", p.default_value);
        auto vals = choc::value::createEmptyArray();
        for (const auto& v : p.values) vals.addArrayElement(v);
        po.addMember("values", vals);
        props.addArrayElement(po);
    }
    root.addMember("props", props);

    auto issues = choc::value::createEmptyArray();
    for (const auto& i : contract.issues) {
        auto io = choc::value::createObject("");
        io.addMember("kind", i.kind);
        io.addMember("detail", i.detail);
        issues.addArrayElement(io);
    }
    root.addMember("issues", issues);
    return choc::json::toString(root, /*pretty=*/true);
}

}  // namespace pulp::design
