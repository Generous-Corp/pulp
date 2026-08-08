#include <pulp/state/param_json.hpp>

#include <pulp/state/store.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pulp::state {
namespace {

const char* kind_name(ParamKind kind) {
    switch (kind) {
    case ParamKind::Continuous:
        return "continuous";
    case ParamKind::Integer:
        return "integer";
    case ParamKind::Toggle:
        return "toggle";
    case ParamKind::Enum:
        return "enum";
    }
    return "continuous";
}

const char* designation_name(ParamDesignation d) {
    switch (d) {
    case ParamDesignation::None:
        return "none";
    case ParamDesignation::Bypass:
        return "bypass";
    case ParamDesignation::Reset:
        return "reset";
    }
    return "none";
}

/// The label for a discrete value, or empty when there isn't one.
std::string label_for(const ParamInfo& info, float value) {
    if (info.value_labels.empty() || !is_discrete_param(info))
        return {};
    const auto index = static_cast<long>(value - info.range.min + 0.5f);
    if (index < 0 || static_cast<std::size_t>(index) >= info.value_labels.size())
        return {};
    return info.value_labels[static_cast<std::size_t>(index)];
}

} // namespace

std::string param_display_text(const ParamInfo& info, float value) {
    // The author's formatter wins — it is what the host already shows, so any
    // other answer here would make the UI disagree with the DAW.
    if (info.to_string)
        return info.to_string(value);

    // A discrete parameter with labels reads as its label. Showing "2" for an
    // enum whose author named its values is a worse default than showing the
    // name they chose.
    if (auto label = label_for(info, value); !label.empty())
        return label;

    // %.3g keeps small integers clean ("440") while trimming float noise. This
    // matches HostParamSurface's long-standing fallback exactly — the two must
    // not disagree about what a value looks like.
    char buf[64];
    if (!info.unit.empty())
        std::snprintf(buf, sizeof(buf), "%.3g %s", value, info.unit.c_str());
    else
        std::snprintf(buf, sizeof(buf), "%.3g", value);
    return std::string(buf);
}

bool param_parse_display_text(const ParamInfo& info, const std::string& text, float& out_value) {
    if (info.from_string) {
        out_value = constrain_param_value(info, info.from_string(text));
        return true;
    }

    if (auto labelled = param_value_for_label(info, text)) {
        out_value = *labelled;
        return true;
    }

    // Numeric with an optional trailing unit: "440 Hz", "-3 dB", "50%".
    // strtof stops at the first character it cannot use, which is exactly the
    // boundary we want; everything after it must be blank or the unit.
    const char* begin = text.c_str();
    char* end = nullptr;
    const float parsed = std::strtof(begin, &end);
    if (end == begin)
        return false; // nothing numeric at all — not a zero

    // Refuse trailing garbage so "12 bananas" is a parse failure rather than 12.
    std::string rest(end);
    const auto first = rest.find_first_not_of(" \t");
    if (first != std::string::npos) {
        rest = rest.substr(first);
        const auto last = rest.find_last_not_of(" \t");
        rest = rest.substr(0, last + 1);
        if (!rest.empty() && rest != info.unit)
            return false;
    }

    out_value = constrain_param_value(info, parsed);
    return true;
}

choc::value::Value param_metadata_to_value(const ParamInfo& info) {
    auto obj = choc::value::createObject("");
    obj.addMember("id", choc::value::createInt64(static_cast<int64_t>(info.id)));
    obj.addMember("name", choc::value::createString(info.name));
    obj.addMember("unit", choc::value::createString(info.unit));
    obj.addMember("min", choc::value::createFloat64(info.range.min));
    obj.addMember("max", choc::value::createFloat64(info.range.max));
    obj.addMember("default", choc::value::createFloat64(info.range.default_value));
    obj.addMember("step", choc::value::createFloat64(info.range.step));
    obj.addMember("skew", choc::value::createFloat64(info.range.skew));
    obj.addMember("symmetricSkew", choc::value::createBool(info.range.symmetric_skew));
    obj.addMember("kind", choc::value::createString(kind_name(info.kind)));
    obj.addMember("groupId", choc::value::createInt32(static_cast<int32_t>(info.group_id)));
    obj.addMember("designation", choc::value::createString(designation_name(info.designation)));
    obj.addMember("isTrigger", choc::value::createBool(info.is_trigger));

    auto labels = choc::value::createEmptyArray();
    for (const auto& label : info.value_labels)
        labels.addArrayElement(choc::value::createString(label));
    obj.addMember("labels", labels);
    return obj;
}

choc::value::Value param_snapshot_to_value(const StateStore& store, const ParamInfo& info) {
    // WIRE-COMPATIBLE with what the inspector protocol has always sent. These
    // field names are load-bearing for existing clients; `display` stays
    // conditional for the same reason. test_param_json.cpp pins the set.
    const float value = store.get_value(info.id);

    auto obj = choc::value::createObject("");
    obj.addMember("id", choc::value::createInt64(static_cast<int64_t>(info.id)));
    obj.addMember("name", choc::value::createString(info.name));
    obj.addMember("unit", choc::value::createString(info.unit));
    obj.addMember("value", choc::value::createFloat64(value));
    obj.addMember("normalized", choc::value::createFloat64(store.get_normalized(info.id)));
    obj.addMember("modulated", choc::value::createFloat64(store.get_modulated(info.id)));
    obj.addMember("default", choc::value::createFloat64(info.range.default_value));
    obj.addMember("min", choc::value::createFloat64(info.range.min));
    obj.addMember("max", choc::value::createFloat64(info.range.max));

    if (auto display = param_display_text(info, value); !display.empty())
        obj.addMember("display", choc::value::createString(display));
    return obj;
}

choc::value::Value param_catalog_snapshot_to_value(const StateStore& store, const ParamInfo& info) {
    auto value = param_metadata_to_value(info);
    const auto snapshot = param_snapshot_to_value(store, info);
    value.addMember("value", snapshot["value"]);
    value.addMember("normalized", snapshot["normalized"]);
    value.addMember("modulated", snapshot["modulated"]);
    if (snapshot.hasObjectMember("display"))
        value.addMember("display", snapshot["display"]);
    return value;
}

} // namespace pulp::state
