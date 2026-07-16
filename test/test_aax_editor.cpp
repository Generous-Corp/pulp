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

#include <map>
#include <string>
#include <utility>
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

/// The AAX parameter manager, reduced to the behavior the mirror contract turns
/// on. Faithful to `AAX_CParameter<float>` on the two points that matter:
///
///   - `SetValueWithFloat` does NOT store the value. It asks the host for it
///     (`AAX_CParameter::SetValue` posts through the automation delegate), so
///     `GetValueAsFloat` keeps returning the old value until the host answers.
///   - the host answers via `AAX_CEffectParameters::UpdateParameterNormalizedValue`,
///     which stores the value FIRST and only then lets the model mirror it into
///     the editor store.
///
/// Both are load-bearing: a mirror that assumed a synchronous write, or that
/// mirrored before storing, would not settle.
class FakeParameterManager {
public:
    FakeParameterManager(const PluginDefinition& definition, state::StateStore& store)
        : definition_(definition), store_(store) {}

    /// `AAX_IParameter::GetValueAsFloat`.
    bool read(const char* aax_id, float& out) const {
        const auto it = values_.find(aax_id);
        if (it == values_.end()) return false;
        out = it->second;
        return true;
    }

    /// `AAX_IParameter::SetValueWithFloat` — a request, not a store.
    void write(const char* aax_id, float value) {
        ++writes;
        pending_.emplace_back(aax_id, value);
    }

    void register_parameter(const std::string& aax_id, float value) { values_[aax_id] = value; }

    /// The host delivering one queued request back as
    /// `UpdateParameterNormalizedValue`. Returns false when nothing was pending.
    bool deliver_one() {
        if (pending_.empty()) return false;
        const auto [aax_id, value] = pending_.front();
        pending_.erase(pending_.begin());
        update_from_host(aax_id, value);
        return true;
    }

    /// An automation move / fader / control surface: a host-originated change no
    /// editor asked for.
    void update_from_host(const std::string& aax_id, float value) {
        values_[aax_id] = value;  // AAX stores before the model mirrors
        state::ParamID id = 0;
        if (param_id_for_aax_id(definition_, aax_id, id)) {
            store_.set_value(id, value);
        }
    }

    /// Run the loop to quiescence, bounded so a non-settling mirror fails the
    /// test instead of hanging it.
    int settle(int max_rounds = 16) {
        int rounds = 0;
        while (deliver_one()) {
            if (++rounds >= max_rounds) break;
        }
        return rounds;
    }

    bool has_pending() const { return !pending_.empty(); }

    int writes = 0;

private:
    const PluginDefinition& definition_;
    state::StateStore& store_;
    std::map<std::string, float> values_;
    std::vector<std::pair<std::string, float>> pending_;
};

/// Register `make_definition()`'s parameters over a plain [0, 1] range.
void add_definition_parameters(state::StateStore& store) {
    for (const state::ParamID id : {kGain, kMix}) {
        state::ParamInfo info;
        info.id = id;
        info.name = "p" + std::to_string(id);
        info.range = state::ParamRange::linear(0.0f, 1.0f, 0.0f);
        store.add_parameter(info);
    }
}

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

// ── ParameterMirror: the editor-store ↔ AAX anti-feedback contract ──────────
//
// The mirror is wired exactly as the adapter wires it: a StateStore value
// listener pushing into a parameter manager that reads back. Only the manager
// is a fake; the store, the listener dispatch, and the mirror are the real code.

namespace {

/// Wires a real StateStore to a ParameterMirror over a FakeParameterManager,
/// the way EffectParameters::ensure_editor_model() does.
struct MirrorFixture {
    PluginDefinition definition = make_definition();
    state::StateStore store;
    FakeParameterManager aax{definition, store};
    ParameterMirror mirror{
        definition,
        [this](const char* id, float& out) { return aax.read(id, out); },
        [this](const char* id, float value) { aax.write(id, value); }};
    state::ListenerToken token;

    MirrorFixture() {
        add_definition_parameters(store);
        for (const auto& binding : definition.parameters) {
            aax.register_parameter(binding.aax_id, 0.0f);
        }
        token = store.add_listener(
            [this](state::ParamID id, float value) {
                mirror.on_store_value_changed(id, value);
            },
            state::ListenerThread::Main);
    }
};

} // namespace

TEST_CASE("AAX mirror writes an editor edit through to the parameter manager",
          "[aax][view-bridge]") {
    MirrorFixture f;

    // The editor moves a knob: the store changes, AAX still holds the old value.
    f.store.set_value(kGain, 0.75f);

    CHECK(f.aax.writes == 1);
    CHECK(f.aax.has_pending());  // AAX_IParameter::SetValue asks the host, not the parameter

    float held = 0.0f;
    REQUIRE(f.aax.read(parameter_id_string(kGain).c_str(), held));
    CHECK(held == 0.0f);  // still the old value until the host answers

    // The host answers; the value lands and the loop closes.
    const int rounds = f.aax.settle();
    CHECK(rounds == 1);
    CHECK(f.aax.writes == 1);  // the return leg did NOT bounce back out
    REQUIRE(f.aax.read(parameter_id_string(kGain).c_str(), held));
    CHECK(held == 0.75f);
    CHECK(f.store.get_value(kGain) == 0.75f);
    CHECK(f.mirror.suppressed_count() == 1);  // the echo, absorbed
}

TEST_CASE("AAX mirror does not bounce a host parameter change back at the host",
          "[aax][view-bridge]") {
    MirrorFixture f;

    // Automation / a fader / a control surface moves the parameter. The model
    // mirrors it into the store, whose listener runs the mirror. Writing back
    // here is what would fight the host for control of the value.
    f.aax.update_from_host(parameter_id_string(kGain), 0.25f);

    CHECK(f.aax.writes == 0);
    CHECK_FALSE(f.aax.has_pending());
    CHECK(f.mirror.suppressed_count() == 1);
    CHECK(f.store.get_value(kGain) == 0.25f);
}

TEST_CASE("AAX mirror settles rather than ping-ponging on a repeated value",
          "[aax][view-bridge]") {
    MirrorFixture f;

    // StateStore::set_value() notifies unconditionally — a write of the value
    // already held still fires the listener. That is precisely why the mirror
    // needs a rule with a fixed point: without one, every echo would re-enter
    // the host at its dispatch rate and never converge.
    f.store.set_value(kGain, 0.5f);
    REQUIRE(f.aax.settle() == 1);
    const int writes_after_first = f.aax.writes;

    for (int i = 0; i < 8; ++i) {
        f.store.set_value(kGain, 0.5f);  // same value, still notifies
    }

    CHECK(f.aax.writes == writes_after_first);  // no new traffic
    CHECK_FALSE(f.aax.has_pending());
    CHECK(f.store.get_value(kGain) == 0.5f);
}

TEST_CASE("AAX mirror leaves a parameter AAX never registered alone",
          "[aax][view-bridge]") {
    MirrorFixture f;

    // Not in the definition at all: no AAX ID to write under.
    CHECK_FALSE(f.mirror.on_store_value_changed(kUnregistered, 0.5f));
    CHECK(f.aax.writes == 0);
    CHECK(f.mirror.suppressed_count() == 0);  // dropped, not counted as an echo
}

TEST_CASE("AAX mirror keeps parameters independent", "[aax][view-bridge]") {
    MirrorFixture f;

    f.store.set_value(kGain, 0.75f);
    f.store.set_value(kMix, 0.25f);
    f.aax.settle();

    float gain = 0.0f;
    float mix = 0.0f;
    REQUIRE(f.aax.read(parameter_id_string(kGain).c_str(), gain));
    REQUIRE(f.aax.read(parameter_id_string(kMix).c_str(), mix));
    CHECK(gain == 0.75f);
    CHECK(mix == 0.25f);
    CHECK(f.aax.writes == 2);
}
