#include <pulp/design/design_knobs.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>

namespace pulp::design {

namespace {

KnobKind kind_from_string(std::string_view s) {
    if (s == "slider") return KnobKind::Slider;
    if (s == "toggle") return KnobKind::Toggle;
    return KnobKind::Enum;  // default and explicit "enum"
}

std::string_view kind_to_string(KnobKind k) {
    switch (k) {
        case KnobKind::Slider: return "slider";
        case KnobKind::Toggle: return "toggle";
        case KnobKind::Enum: return "enum";
    }
    return "enum";
}

/// Parse `s` as a JSON number. Returns nullopt when `s` is empty, is not exactly
/// one JSON number, or has trailing bytes — so "0.5x" and "" both reject rather
/// than silently becoming 0. Parsing through choc (wrapped in a one-element array
/// so a bare scalar is accepted) keeps this locale-independent: a JSON number is
/// always '.'-form, unlike std::strtod, which honors LC_NUMERIC and would stop at
/// the '.' of "0.5" under a comma-decimal locale.
std::optional<double> parse_number(std::string_view s) {
    if (s.empty()) return std::nullopt;
    try {
        auto wrapped = choc::json::parse("[" + std::string(s) + "]");
        if (wrapped.isArray() && wrapped.size() == 1 &&
            (wrapped[0].isInt() || wrapped[0].isFloat()))
            return wrapped[0].getWithDefault<double>(0.0);
    } catch (...) {
    }
    return std::nullopt;
}

/// The JSON text of one choc value member, compact (no line breaks). This is the
/// exact serialization stored in KnobWrite::json_value.
std::string member_json_text(const choc::value::ValueView& v) {
    return choc::json::toString(v);
}

std::vector<KnobWrite> parse_writes(const choc::value::ValueView& arr) {
    std::vector<KnobWrite> writes;
    if (!arr.isArray()) return writes;
    for (uint32_t i = 0; i < arr.size(); ++i) {
        auto w = arr[i];
        if (!w.isObject() || !w.hasObjectMember("key") || !w.hasObjectMember("value"))
            continue;  // a write missing its key or value is skipped, not fatal
        auto key = w["key"];
        if (!key.isString()) continue;
        writes.push_back({std::string(key.getString()), member_json_text(w["value"])});
    }
    return writes;
}

}  // namespace

std::optional<KnobSchema> parse_knob_schema(std::string_view json) {
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (...) {
        return std::nullopt;
    }
    if (!root.isObject() || !root.hasObjectMember("knobs") || !root["knobs"].isArray())
        return std::nullopt;

    KnobSchema schema;
    auto knobs = root["knobs"];
    for (uint32_t i = 0; i < knobs.size(); ++i) {
        auto k = knobs[i];
        if (!k.isObject() || !k.hasObjectMember("id")) continue;
        auto id = k["id"];
        if (!id.isString() || id.getString().empty()) continue;

        KnobSpec spec;
        spec.id = std::string(id.getString());
        spec.label = k.hasObjectMember("label") && k["label"].isString()
                         ? std::string(k["label"].getString())
                         : spec.id;
        spec.kind = k.hasObjectMember("kind") && k["kind"].isString()
                        ? kind_from_string(k["kind"].getString())
                        : KnobKind::Enum;

        if (k.hasObjectMember("positions") && k["positions"].isArray()) {
            auto positions = k["positions"];
            for (uint32_t p = 0; p < positions.size(); ++p) {
                auto pos = positions[p];
                if (!pos.isObject()) continue;
                KnobPosition kp;
                kp.label = pos.hasObjectMember("label") && pos["label"].isString()
                               ? std::string(pos["label"].getString())
                               : std::string();
                kp.at = pos.hasObjectMember("at") ? pos["at"].getWithDefault<double>(0.0) : 0.0;
                kp.writes = pos.hasObjectMember("writes") ? parse_writes(pos["writes"])
                                                          : std::vector<KnobWrite>{};
                spec.positions.push_back(std::move(kp));
            }
        }

        // A slider selects by nearest anchor, so keep its positions ordered by
        // `at`. Toggle/Enum select by label/index, so authored order is kept.
        if (spec.kind == KnobKind::Slider) {
            std::stable_sort(spec.positions.begin(), spec.positions.end(),
                             [](const KnobPosition& a, const KnobPosition& b) { return a.at < b.at; });
        }
        schema.knobs.push_back(std::move(spec));
    }
    return schema;
}

std::string knob_schema_to_json(const KnobSchema& schema) {
    auto root = choc::value::createObject("");
    auto knobs = choc::value::createEmptyArray();
    for (const auto& spec : schema.knobs) {
        auto k = choc::value::createObject("");
        k.setMember("id", spec.id);
        k.setMember("label", spec.label);
        k.setMember("kind", std::string(kind_to_string(spec.kind)));
        auto positions = choc::value::createEmptyArray();
        for (const auto& pos : spec.positions) {
            auto po = choc::value::createObject("");
            po.setMember("label", pos.label);
            po.setMember("at", pos.at);
            auto writes = choc::value::createEmptyArray();
            for (const auto& w : pos.writes) {
                auto wo = choc::value::createObject("");
                wo.setMember("key", w.key);
                // json_value is already JSON text; re-parse so it embeds as a
                // real value, not a doubly-quoted string. choc::json::parse
                // rejects a bare top-level scalar (false, "airy", 8), so wrap it
                // in a one-element array and take element 0. Fall back to the raw
                // string only if it is somehow not valid JSON at all.
                try {
                    auto wrapped = choc::json::parse("[" + w.json_value + "]");
                    if (wrapped.isArray() && wrapped.size() == 1)
                        wo.setMember("value", wrapped[0]);
                    else
                        wo.setMember("value", w.json_value);
                } catch (...) {
                    wo.setMember("value", w.json_value);
                }
                writes.addArrayElement(wo);
            }
            po.setMember("writes", writes);
            positions.addArrayElement(po);
        }
        k.setMember("positions", positions);
        knobs.addArrayElement(k);
    }
    root.setMember("knobs", knobs);
    return choc::json::toString(root, true);
}

const KnobSpec* find_knob(const KnobSchema& schema, std::string_view id) {
    for (const auto& k : schema.knobs)
        if (k.id == id) return &k;
    return nullptr;
}

const KnobPosition* select_position(const KnobSpec& knob, std::string_view value) {
    if (knob.positions.empty()) return nullptr;

    if (knob.kind == KnobKind::Slider) {
        auto v = parse_number(value);
        if (!v) return nullptr;
        const KnobPosition* best = nullptr;
        double best_dist = 0.0;
        for (const auto& p : knob.positions) {
            double d = std::fabs(p.at - *v);
            if (!best || d < best_dist) {
                best = &p;
                best_dist = d;
            }
        }
        return best;
    }

    // Toggle / Enum: match a label first (a caller nearly always sends a label).
    for (const auto& p : knob.positions)
        if (p.label == value) return &p;

    // Fall back to a 0-based index so a caller can drive by position ordinal.
    if (auto idx = parse_number(value)) {
        double d = *idx;
        if (d >= 0.0 && std::floor(d) == d) {
            auto i = static_cast<size_t>(d);
            if (i < knob.positions.size()) return &knob.positions[i];
        }
    }
    return nullptr;
}

WriteTarget classify_write(const KnobWrite& write,
                           const std::vector<std::string>& theme_tokens) {
    for (const auto& name : theme_tokens)
        if (name == write.key) return WriteTarget::ThemeToken;
    return WriteTarget::LocalBlock;
}

std::optional<KnobEffect> resolve_knob(const KnobSpec& knob, std::string_view value,
                                       const std::vector<std::string>& theme_tokens) {
    const KnobPosition* pos = select_position(knob, value);
    if (!pos) return std::nullopt;
    KnobEffect effect;
    for (const auto& w : pos->writes) {
        if (classify_write(w, theme_tokens) == WriteTarget::ThemeToken)
            effect.token_writes.push_back(w);
        else
            effect.local_writes.push_back(w);
    }
    return effect;
}

std::optional<KnobApply> apply_knob(std::string_view artifact, const KnobSpec& knob,
                                    std::string_view value,
                                    const std::vector<std::string>& theme_tokens) {
    auto effect = resolve_knob(knob, value, theme_tokens);
    if (!effect) return std::nullopt;

    // Token-only change: no block needed, artifact is untouched.
    if (effect->local_writes.empty())
        return KnobApply{std::string(artifact), std::move(effect->token_writes)};

    auto params = read_edit_block(artifact);
    if (!params) return std::nullopt;  // local writes need a block to anchor to

    // Update in place / append, preserving existing key order.
    for (const auto& w : effect->local_writes) {
        auto it = std::find_if(params->begin(), params->end(),
                               [&](const TweakParam& p) { return p.key == w.key; });
        if (it != params->end())
            it->json_value = w.json_value;
        else
            params->push_back({w.key, w.json_value});
    }

    auto rewritten = rewrite_edit_block(artifact, *params);
    if (!rewritten || !rewritten->outside_bytes_intact) return std::nullopt;
    return KnobApply{std::move(rewritten->text), std::move(effect->token_writes)};
}

KnobSpec builtin_theme_flip() {
    KnobSpec spec;
    spec.id = "theme";
    spec.label = "Theme";
    spec.kind = KnobKind::Enum;
    spec.positions.push_back({"light", 0.0, {{"appearance", "\"light\""}}});
    spec.positions.push_back({"dark", 0.0, {{"appearance", "\"dark\""}}});
    return spec;
}

}  // namespace pulp::design
