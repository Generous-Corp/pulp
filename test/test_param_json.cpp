// The one parameter payload, and the pin that keeps it one.
//
// param_json is deliberately the only place a parameter becomes JSON: the
// scripted-UI bridge and the dev inspector both call it, so there is no second
// implementation that could drift from the first. What a single implementation
// cannot prevent is someone renaming a field, or making a conditional one
// unconditional, and silently breaking every existing inspector client. These
// tests pin the field set so that becomes a failing test instead.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <pulp/state/param_json.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace pulp::state;

namespace {

std::set<std::string> member_names(const choc::value::Value& v) {
    std::set<std::string> names;
    for (uint32_t i = 0; i < v.size(); ++i)
        names.insert(std::string(v.getObjectMemberAt(i).name));
    return names;
}

const ParamInfo& info_for(const StateStore& store, ParamID id) {
    for (std::size_t i = 0; i < store.param_count(); ++i)
        if (store.all_params()[i].id == id) return store.all_params()[i];
    throw std::runtime_error("no such param");
}

} // namespace

TEST_CASE("the inspector snapshot keeps its established wire field set",
          "[state][param-json]") {
    StateStore store;
    store.add_parameter({.id = 1, .name = "Cutoff", .unit = "Hz",
                         .range = {.min = 20.0f, .max = 20000.0f, .default_value = 1000.0f}});

    const auto snapshot = param_snapshot_to_value(store, info_for(store, 1));

    // These names are load-bearing for clients already reading the inspector
    // protocol. Renaming one is a breaking change, not a refactor — if this
    // fails, you changed a wire contract.
    const std::set<std::string> required{"id",   "name", "unit", "value", "normalized",
                                         "modulated", "default", "min", "max", "display"};
    CHECK(member_names(snapshot) == required);

    CHECK(snapshot["name"].getString() == "Cutoff");
    CHECK(snapshot["min"].getFloat64() == Catch::Approx(20.0));
    CHECK(snapshot["max"].getFloat64() == Catch::Approx(20000.0));
    CHECK(snapshot["default"].getFloat64() == Catch::Approx(1000.0));
}

TEST_CASE("parameter JSON preserves the complete uint32 identifier domain",
          "[state][param-json][boundary]") {
    StateStore store;
    constexpr ParamID high_id = std::numeric_limits<ParamID>::max();
    store.add_parameter({.id = high_id, .name = "High ID",
                         .range = {.min = 0.0f, .max = 1.0f}});

    const auto& info = info_for(store, high_id);
    const auto metadata = param_metadata_to_value(info);
    const auto snapshot = param_snapshot_to_value(store, info);

    REQUIRE(metadata["id"].isInt());
    REQUIRE(snapshot["id"].isInt());
    CHECK(metadata["id"].getInt64() == static_cast<std::int64_t>(high_id));
    CHECK(snapshot["id"].getInt64() == static_cast<std::int64_t>(high_id));
}

TEST_CASE("display stays conditional, exactly as the inspector always sent it",
          "[state][param-json]") {
    StateStore store;
    // A parameter whose display text is empty: no unit, and a formatter that
    // returns nothing. The historical protocol omits the member entirely rather
    // than sending "", and a client may be relying on that.
    ParamInfo silent{};
    silent.id = 7;
    silent.name = "Quiet";
    silent.range = {.min = 0.0f, .max = 1.0f};
    silent.to_string = [](float) { return std::string{}; };
    store.add_parameter(silent);

    const auto snapshot = param_snapshot_to_value(store, info_for(store, 7));
    CHECK(member_names(snapshot).count("display") == 0);
}

TEST_CASE("metadata carries the static shape a UI needs to build a control",
          "[state][param-json]") {
    StateStore store;
    ParamInfo mode{};
    mode.id = 3;
    mode.name = "Mode";
    mode.kind = ParamKind::Enum;
    mode.value_labels = {"Low", "Band", "High"};
    mode.range = {.min = 0.0f, .max = 2.0f, .default_value = 1.0f, .step = 1.0f};
    store.add_parameter(mode);

    const auto meta = param_metadata_to_value(info_for(store, 3));

    const std::set<std::string> required{
        "id",   "name",  "unit",    "min",    "max",         "default", "step",
        "skew", "symmetricSkew", "kind", "labels", "groupId", "designation", "isTrigger"};
    CHECK(member_names(meta) == required);

    CHECK(meta["kind"].getString() == "enum");
    REQUIRE(meta["labels"].size() == 3);
    CHECK(meta["labels"][1].getString() == "Band");
    CHECK(meta["step"].getFloat64() == Catch::Approx(1.0));
}

TEST_CASE("metadata and snapshot agree about the fields they share",
          "[state][param-json]") {
    StateStore store;
    store.add_parameter({.id = 5, .name = "Drive", .unit = "dB",
                         .range = {.min = -12.0f, .max = 12.0f, .default_value = 0.0f}});

    const auto& info = info_for(store, 5);
    const auto meta = param_metadata_to_value(info);
    const auto snap = param_snapshot_to_value(store, info);

    // Both payloads describe the same parameter. A consumer that reads `min`
    // from one and `max` from the other must not get an inconsistent picture —
    // which is the whole argument for a single serializer.
    for (const char* shared : {"id", "name", "unit", "min", "max", "default"}) {
        INFO("shared field: " << shared);
        REQUIRE(meta.hasObjectMember(shared));
        REQUIRE(snap.hasObjectMember(shared));
        CHECK(choc::json::toString(meta[shared]) == choc::json::toString(snap[shared]));
    }
}

TEST_CASE("display text prefers the author's formatter, then a label, then units",
          "[state][param-json]") {
    ParamInfo authored{};
    authored.name = "A";
    authored.unit = "Hz";
    authored.range = {.min = 0.0f, .max = 1000.0f};
    authored.to_string = [](float v) { return std::string("<") + std::to_string(int(v)) + ">"; };
    CHECK(param_display_text(authored, 440.0f) == "<440>");

    ParamInfo labelled{};
    labelled.name = "B";
    labelled.kind = ParamKind::Enum;
    labelled.value_labels = {"Low", "Band", "High"};
    labelled.range = {.min = 0.0f, .max = 2.0f, .step = 1.0f};
    // An enum reads as the name its author chose, not as "2".
    CHECK(param_display_text(labelled, 2.0f) == "High");

    ParamInfo plain{};
    plain.name = "C";
    plain.unit = "Hz";
    plain.range = {.min = 0.0f, .max = 1000.0f};
    CHECK(param_display_text(plain, 440.0f) == "440 Hz");

    ParamInfo unitless{};
    unitless.name = "D";
    unitless.range = {.min = 0.0f, .max = 1.0f};
    CHECK(param_display_text(unitless, 0.5f) == "0.5");
}

TEST_CASE("parsing distinguishes an unparseable string from a zero",
          "[state][param-json]") {
    ParamInfo freq{};
    freq.name = "Cutoff";
    freq.unit = "Hz";
    freq.range = {.min = 20.0f, .max = 20000.0f};

    float out = -1.0f;
    REQUIRE(param_parse_display_text(freq, "440", out));
    CHECK(out == Catch::Approx(440.0f));

    out = -1.0f;
    REQUIRE(param_parse_display_text(freq, "440 Hz", out));
    CHECK(out == Catch::Approx(440.0f));

    // Out of range is clamped to the declared range, not rejected.
    out = -1.0f;
    REQUIRE(param_parse_display_text(freq, "99999", out));
    CHECK(out == Catch::Approx(20000.0f));

    // The important one: nonsense must FAIL rather than quietly yielding 0,
    // which a caller would happily store as a real value.
    out = 123.0f;
    CHECK_FALSE(param_parse_display_text(freq, "banana", out));
    CHECK(out == Catch::Approx(123.0f));  // untouched on failure
    CHECK_FALSE(param_parse_display_text(freq, "", out));
    CHECK_FALSE(param_parse_display_text(freq, "12 bananas", out));

    // And a genuine zero still parses as a zero.
    ParamInfo gain{};
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {.min = -60.0f, .max = 12.0f};
    out = -1.0f;
    REQUIRE(param_parse_display_text(gain, "0 dB", out));
    CHECK(out == Catch::Approx(0.0f));
}

TEST_CASE("format and parse round-trip through each other",
          "[state][param-json]") {
    ParamInfo freq{};
    freq.name = "Cutoff";
    freq.unit = "Hz";
    freq.range = {.min = 20.0f, .max = 20000.0f};

    // The pair is only useful if a UI can render a value, hand the text back to
    // the user to edit, and read the same number out again.
    for (float v : {20.0f, 440.0f, 1000.0f, 20000.0f}) {
        const auto text = param_display_text(freq, v);
        float back = -1.0f;
        INFO("value " << v << " formatted as '" << text << "'");
        REQUIRE(param_parse_display_text(freq, text, back));
        CHECK(back == Catch::Approx(v).epsilon(0.01));
    }

    ParamInfo mode{};
    mode.name = "Mode";
    mode.kind = ParamKind::Enum;
    mode.value_labels = {"Low", "Band", "High"};
    mode.range = {.min = 0.0f, .max = 2.0f, .step = 1.0f};
    for (float v : {0.0f, 1.0f, 2.0f}) {
        const auto text = param_display_text(mode, v);
        float back = -1.0f;
        INFO("enum " << v << " formatted as '" << text << "'");
        REQUIRE(param_parse_display_text(mode, text, back));
        CHECK(back == Catch::Approx(v));
    }
}
