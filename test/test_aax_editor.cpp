// AAX editor-support logic: parameter-gesture routing and the editor sizing
// contract. Both are SDK-free by construction, so this suite runs everywhere —
// including the public CI lanes, which never have the developer-supplied Avid
// SDK. The thin AAX_CEffectGUI shell they back is covered by the SDK-gated
// suite.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_editor.hpp>
#include <pulp/format/aax_model.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;
using namespace pulp::format::aax;

namespace {

/// Records the touch/release calls a GestureRouter emits, in order, so a test
/// can assert the exact automation traffic AAX would see.
struct GestureLog {
    std::vector<std::string> calls;

    GestureRouter::Sink touch_sink() {
        return [this](const char* id) { calls.push_back(std::string("touch:") + id); };
    }
    GestureRouter::Sink release_sink() {
        return [this](const char* id) { calls.push_back(std::string("release:") + id); };
    }
};

/// Definition with two parameters whose AAX IDs follow the adapter's
/// `parameter_id_string` convention.
PluginDefinition make_definition() {
    PluginDefinition definition;
    for (const state::ParamID id : {state::ParamID{1}, state::ParamID{7}}) {
        ParameterBinding binding;
        binding.id = id;
        binding.aax_id = parameter_id_string(id);
        binding.name = "p" + std::to_string(id);
        definition.parameters.push_back(std::move(binding));
    }
    return definition;
}

constexpr state::ParamID kGain = 1;
constexpr state::ParamID kMix = 7;
constexpr state::ParamID kUnregistered = 99;

} // namespace

TEST_CASE("AAX gesture router maps ParamID to the registered AAX parameter ID",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    REQUIRE(router.aax_id_for(kGain) != nullptr);
    CHECK(*router.aax_id_for(kGain) == parameter_id_string(kGain));
    CHECK(*router.aax_id_for(kMix) == parameter_id_string(kMix));
    CHECK(router.aax_id_for(kUnregistered) == nullptr);
}

TEST_CASE("AAX gesture router emits a touch/release pair per gesture",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    CHECK(router.begin(kGain));
    CHECK(router.end(kGain));

    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0] == "touch:" + parameter_id_string(kGain));
    CHECK(log.calls[1] == "release:" + parameter_id_string(kGain));
    CHECK(router.touched_count() == 0);
}

TEST_CASE("AAX gesture router keeps a re-entered drag to one automation record",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    CHECK(router.begin(kGain));
    CHECK_FALSE(router.begin(kGain));  // second begin emits nothing
    CHECK(router.is_touched(kGain));
    CHECK(router.touched_count() == 1);

    CHECK(router.end(kGain));
    CHECK_FALSE(router.end(kGain));  // second end emits nothing

    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0] == "touch:" + parameter_id_string(kGain));
    CHECK(log.calls[1] == "release:" + parameter_id_string(kGain));
}

TEST_CASE("AAX gesture router ignores a release with no matching touch",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    CHECK_FALSE(router.end(kGain));
    CHECK(log.calls.empty());
    CHECK(router.unknown_count() == 0);  // known param, just not touched
}

TEST_CASE("AAX gesture router drops gestures for unregistered parameters",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    CHECK_FALSE(router.begin(kUnregistered));
    CHECK_FALSE(router.end(kUnregistered));

    CHECK(log.calls.empty());
    CHECK(router.unknown_count() == 2);
    CHECK(router.touched_count() == 0);
}

TEST_CASE("AAX gesture router interleaves independent parameter gestures",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    CHECK(router.begin(kGain));
    CHECK(router.begin(kMix));
    CHECK(router.touched_count() == 2);
    CHECK(router.end(kGain));
    CHECK(router.end(kMix));

    REQUIRE(log.calls.size() == 4);
    CHECK(log.calls[0] == "touch:" + parameter_id_string(kGain));
    CHECK(log.calls[1] == "touch:" + parameter_id_string(kMix));
    CHECK(log.calls[2] == "release:" + parameter_id_string(kGain));
    CHECK(log.calls[3] == "release:" + parameter_id_string(kMix));
}

TEST_CASE("AAX gesture router releases parameters still held at teardown",
          "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureLog log;
    GestureRouter router(definition, log.touch_sink(), log.release_sink());

    CHECK(router.begin(kGain));
    CHECK(router.begin(kMix));
    log.calls.clear();

    // Closing the editor mid-drag must not strand an open automation record.
    router.release_all();

    CHECK(router.touched_count() == 0);
    REQUIRE(log.calls.size() == 2);
    for (const auto& call : log.calls) {
        CHECK(call.rfind("release:", 0) == 0);
    }
    // Idempotent: nothing left to release.
    log.calls.clear();
    router.release_all();
    CHECK(log.calls.empty());
}

TEST_CASE("AAX gesture router tolerates absent sinks", "[aax][view-bridge]") {
    const auto definition = make_definition();
    GestureRouter router(definition, nullptr, nullptr);

    CHECK(router.begin(kGain));
    CHECK(router.is_touched(kGain));
    CHECK(router.end(kGain));
    CHECK(router.touched_count() == 0);
}

TEST_CASE("AAX parameter IDs resolve back to their store ParamID",
          "[aax][view-bridge]") {
    const auto definition = make_definition();

    state::ParamID resolved = 0;
    REQUIRE(param_id_for_aax_id(definition, parameter_id_string(kGain), resolved));
    CHECK(resolved == kGain);
    REQUIRE(param_id_for_aax_id(definition, parameter_id_string(kMix), resolved));
    CHECK(resolved == kMix);

    // AAX's own master-bypass control has no Pulp store parameter behind it.
    CHECK_FALSE(param_id_for_aax_id(definition, "Bypass", resolved));
    CHECK_FALSE(param_id_for_aax_id(definition, "", resolved));
}

TEST_CASE("AAX editor size plan pins the viewport for a fixed-size editor",
          "[aax][view-bridge]") {
    ViewSize hints;
    hints.preferred_width = 800;
    hints.preferred_height = 400;
    // min stays 0 — the fixed-size convention CLAP and VST3 share.

    const auto plan = plan_editor_size(hints);
    CHECK(plan.width == 800);
    CHECK(plan.height == 400);
    CHECK_FALSE(plan.resizable);
    CHECK(plan.pin_viewport);
    CHECK(plan.aspect_ratio == 2.0f);
    // A surface that cannot reflow must not invite the host to shrink it.
    CHECK(plan.min_width == 800);
    CHECK(plan.min_height == 400);
}

TEST_CASE("AAX editor size plan locks the aspect for a design-import editor",
          "[aax][view-bridge]") {
    ViewSize hints;
    hints.preferred_width = 600;
    hints.preferred_height = 300;
    hints.min_width = 300;
    hints.min_height = 150;
    hints.aspect_ratio = 2.0;

    const auto plan = plan_editor_size(hints);
    CHECK(plan.resizable);
    CHECK(plan.pin_viewport);
    CHECK(plan.aspect_ratio == 2.0f);
}

TEST_CASE("AAX editor size plan lets a free-resize editor reflow",
          "[aax][view-bridge]") {
    ViewSize hints;
    hints.preferred_width = 640;
    hints.preferred_height = 480;
    hints.min_width = 320;
    hints.min_height = 240;
    hints.aspect_ratio = 0.0;  // any ratio

    const auto plan = plan_editor_size(hints);
    CHECK(plan.width == 640);
    CHECK(plan.height == 480);
    CHECK(plan.resizable);
    CHECK_FALSE(plan.pin_viewport);  // no viewport pin, no aspect lock
    CHECK(plan.aspect_ratio == 0.0f);
    // A resizable editor reports the plugin's own floor, not its design size.
    CHECK(plan.min_width == 320);
    CHECK(plan.min_height == 240);
}

TEST_CASE("AAX editor size plan never divides by a zero design size",
          "[aax][view-bridge]") {
    ViewSize hints;
    hints.preferred_width = 0;
    hints.preferred_height = 0;

    const auto plan = plan_editor_size(hints);
    CHECK_FALSE(plan.pin_viewport);
    CHECK(plan.aspect_ratio == 0.0f);
}
