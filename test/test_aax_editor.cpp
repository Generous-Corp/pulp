// AAX editor-support logic: parameter-gesture routing and the editor sizing
// contract. Both are SDK-free by construction, so this suite runs everywhere —
// including the public CI lanes, which never have the developer-supplied Avid
// SDK. The thin AAX_CEffectGUI shell they back is covered by the SDK-gated
// suite.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Before the using-directives below: Vellum's headers name `vellum::`
// unqualified, which `using namespace pulp` would make ambiguous.
#if defined(PULP_TEST_AAX_VELLUM)
#include <pulp/vellum/editor.hpp>
#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/view.hpp>
#endif

#include <pulp/format/aax_editor.hpp>
#include <pulp/format/aax_model.hpp>
#include <pulp/format/aax_native_view.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <limits>
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
/// `parameter_id_string` convention. The range is what the adapter hands its
/// taper delegate, so it has to be the same one the store is registered over.
PluginDefinition make_definition(
    state::ParamRange range = state::ParamRange::linear(0.0f, 1.0f, 0.0f)) {
    PluginDefinition definition;
    for (const state::ParamID id : {state::ParamID{1}, state::ParamID{7}}) {
        ParameterBinding binding;
        binding.id = id;
        binding.aax_id = parameter_id_string(id);
        binding.name = "p" + std::to_string(id);
        binding.range = range;
        definition.parameters.push_back(std::move(binding));
    }
    return definition;
}

constexpr state::ParamID kGain = 1;
constexpr state::ParamID kMix = 7;
constexpr state::ParamID kUnregistered = 99;

/// The AAX parameter manager, reduced to the behavior the mirror contract turns
/// on. Faithful to `AAX_CParameter<float>` on the three points that matter:
///
///   - `SetValueWithFloat` does NOT store the value. `AAX_CParameter::SetValue`
///     posts `RealToNormalized(value)` through the automation delegate and
///     stores nothing, so `GetValueAsFloat` keeps returning the old value until
///     the host answers.
///   - what travels to the host and back is therefore a *normalized* value, and
///     the parameter stores `NormalizedToReal` of it. Both legs go through the
///     taper, so the value AAX settles on is generally NOT the value the mirror
///     asked for.
///   - the host answers via `AAX_CEffectParameters::UpdateParameterNormalizedValue`,
///     which stores the value FIRST and only then lets the model mirror it into
///     the editor store — with `set_normalized`, the way the adapter does, so
///     the store applies its own constraint on top.
///
/// All three are load-bearing. A fake that echoed the real value back verbatim,
/// or pushed it with `set_value`, would drop both lossy transforms and could
/// only ever confirm whatever rule the mirror already implemented.
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

    /// `AAX_IParameter::SetValueWithFloat` — a request, not a store. What gets
    /// queued is the normalized token `SetValue` posts, not `value` itself.
    bool write(const char* aax_id, float value) {
        ++writes;
        pending_.emplace_back(aax_id, taper_real_to_normalized(range_for(aax_id), value));
        return true;
    }

    void register_parameter(const std::string& aax_id, float value) { values_[aax_id] = value; }

    /// The host delivering one queued request back as
    /// `UpdateParameterNormalizedValue`. Returns false when nothing was pending.
    bool deliver_one() {
        if (pending_.empty()) return false;
        const auto [aax_id, normalized] = pending_.front();
        pending_.erase(pending_.begin());
        update_from_host(aax_id, normalized);
        return true;
    }

    /// An automation move / fader / control surface: a host-originated change no
    /// editor asked for. AAX hands the model a normalized value.
    void update_from_host(const std::string& aax_id, double normalized) {
        // AAX stores NormalizedToReal(N) before the model mirrors anything.
        values_[aax_id] = taper_normalized_to_real(range_for(aax_id), normalized);
        state::ParamID id = 0;
        if (param_id_for_aax_id(definition_, aax_id, id)) {
            store_.set_normalized(id, static_cast<float>(normalized));
        }
    }

    /// Move a parameter to a real value the way a host would: normalize it first
    /// and deliver that, so the taper round trip is not skipped.
    void update_from_host_real(const std::string& aax_id, float value) {
        update_from_host(aax_id, taper_real_to_normalized(range_for(aax_id), value));
    }

    /// The value AAX holds for `aax_id`, or NaN when it is not registered.
    float held(const std::string& aax_id) const {
        const auto it = values_.find(aax_id);
        return it == values_.end() ? std::numeric_limits<float>::quiet_NaN() : it->second;
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
    const state::ParamRange& range_for(const std::string& aax_id) const {
        for (const auto& binding : definition_.parameters) {
            if (binding.aax_id == aax_id) return binding.range;
        }
        static const state::ParamRange fallback{};
        return fallback;
    }

    const PluginDefinition& definition_;
    state::StateStore& store_;
    std::map<std::string, float> values_;
    std::vector<std::pair<std::string, double>> pending_;
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

TEST_CASE("AAX editor size plan pins an authored design at its design box",
          "[aax][view-bridge][resize]") {
    // An imported 1320x860 design that opens in a 990x645 host frame: the
    // host size is what GetViewSize reports, but the root must lay out in the
    // authored box so paint scales it 0.75x into the frame. Pinning the
    // preferred size instead paints the authored tree at scale 1 and shows
    // only its top-left corner.
    ViewSize hints;
    hints.preferred_width = 990;
    hints.preferred_height = 645;
    hints.min_width = 792;
    hints.min_height = 516;
    hints.max_width = 2640;
    hints.max_height = 1720;
    hints.aspect_ratio = 1320.0 / 860.0;
    hints.design_width = 1320;
    hints.design_height = 860;

    const auto plan = plan_editor_size(hints);
    CHECK(plan.width == 990);
    CHECK(plan.height == 645);
    CHECK(plan.resizable);
    CHECK(plan.pin_viewport);
    CHECK(plan.design_width == 1320);
    CHECK(plan.design_height == 860);
    CHECK(plan.aspect_ratio == Catch::Approx(1320.0f / 860.0f));
    // The same numbers every other adapter hands set_design_viewport.
    CHECK(plan.design_width == design_viewport_width(hints));
    CHECK(plan.design_height == design_viewport_height(hints));
    CHECK(plan.pin_viewport == should_pin_design_viewport(hints));
}

TEST_CASE("AAX editor size plan falls back to the preferred box without an authored design",
          "[aax][view-bridge][resize]") {
    ViewSize hints;
    hints.preferred_width = 800;
    hints.preferred_height = 400;

    const auto plan = plan_editor_size(hints);
    CHECK(plan.pin_viewport);
    CHECK(plan.design_width == 800);
    CHECK(plan.design_height == 400);
}

TEST_CASE("AAX editor size plan leaves a free-resize editor without a design box",
          "[aax][view-bridge][resize]") {
    ViewSize hints;
    hints.preferred_width = 640;
    hints.preferred_height = 480;
    hints.min_width = 320;
    hints.min_height = 240;
    hints.aspect_ratio = 0.0;
    hints.design_width = 1280;
    hints.design_height = 960;

    const auto plan = plan_editor_size(hints);
    CHECK_FALSE(plan.pin_viewport);
    CHECK(plan.design_width == 0);
    CHECK(plan.design_height == 0);
    CHECK(plan.aspect_ratio == 0.0f);
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
        [this](const char* id, float value) { return aax.write(id, value); }};
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
    f.aax.update_from_host_real(parameter_id_string(kGain), 0.25f);

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

// ── Convergence: the property the whole contract exists to provide ──────────
//
// The mirror sits between two lossy transforms — AAX's taper round trip and the
// store's own constraint — so "the two values are equal" is not a question it
// can ask. These drive the real round trip to quiescence and assert it stops.

namespace {

/// One parameter, wired end to end: real StateStore, real ParameterMirror, real
/// taper math, and a host that echoes perfectly. Runs the loop until it settles
/// or exceeds `max_writes`, and reports which.
struct ConvergenceProbe {
    long long writes = 0;
    bool converged = false;
    float aax_value = 0.0f;
    float store_value = 0.0f;
    std::uint64_t refused = 0;
};

ConvergenceProbe run_to_quiescence(state::ParamRange range,
                                   state::ParamKind kind,
                                   double start_normalized,
                                   long long max_writes = 100) {
    const auto definition = make_definition(range);
    state::StateStore store;
    state::ParamInfo info;
    info.id = kGain;
    info.name = "p";
    info.range = range;
    info.kind = kind;
    store.add_parameter(info);

    FakeParameterManager aax{definition, store};
    for (const auto& binding : definition.parameters) {
        aax.register_parameter(binding.aax_id, taper_normalized_to_real(range, 0.0));
    }

    ConvergenceProbe probe;
    ParameterMirror mirror{
        definition,
        [&aax](const char* id, float& out) { return aax.read(id, out); },
        [&aax, &probe, max_writes](const char* id, float value) {
            if (++probe.writes > max_writes) return false;
            return aax.write(id, value);
        }};
    const auto token = store.add_listener(
        [&mirror](state::ParamID id, float value) { mirror.on_store_value_changed(id, value); },
        state::ListenerThread::Main);

    // The host moves the parameter, then answers every request it gets back.
    aax.update_from_host(parameter_id_string(kGain), start_normalized);
    while (aax.has_pending() && probe.writes <= max_writes) {
        aax.deliver_one();
    }

    probe.converged = probe.writes <= max_writes && !aax.has_pending();
    probe.aax_value = aax.held(parameter_id_string(kGain));
    probe.store_value = store.get_value(kGain);
    probe.refused = mirror.refused_count();
    return probe;
}

struct NamedRange {
    const char* name;
    state::ParamRange range;
};

/// Ranges a real plugin plausibly declares: linear and skewed, stepped and not.
/// The skewed ones matter most — `pow()` round trips are far less forgiving than
/// linear ones, which survive on float accidents like `float(1/3) * 3 == 1.0f`.
std::vector<NamedRange> convergence_ranges() {
    return {
        {"[0,1] lin step=0", {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,3] lin step=0", {0.0f, 3.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,10] lin step=0", {0.0f, 10.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,7] lin step=0", {0.0f, 7.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[1,128] lin step=0", {1.0f, 128.0f, 1.0f, 0.0f, 1.0f, false}},
        {"[-24,24] lin step=0", {-24.0f, 24.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,5] lin step=0", {0.0f, 5.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,6] lin step=0", {0.0f, 6.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,9] lin step=0", {0.0f, 9.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,11] lin step=0", {0.0f, 11.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,100] lin step=0", {0.0f, 100.0f, 0.0f, 0.0f, 1.0f, false}},
        {"[0,1] lin step=0.1", {0.0f, 1.0f, 0.0f, 0.1f, 1.0f, false}},
        {"[0,1] lin step=0.3", {0.0f, 1.0f, 0.0f, 0.3f, 1.0f, false}},
        {"[20,20k] skew=0.3 step=0", {20.0f, 20000.0f, 440.0f, 0.0f, 0.3f, false}},
        {"[0,3] skew=0.5 step=0", {0.0f, 3.0f, 0.0f, 0.0f, 0.5f, false}},
        {"[0,10] skew=2 step=0", {0.0f, 10.0f, 0.0f, 0.0f, 2.0f, false}},
        {"[0,4] skew=0.25 step=0", {0.0f, 4.0f, 0.0f, 0.0f, 0.25f, false}},
        {"[-1,1] symskew=2 step=0", {-1.0f, 1.0f, 0.0f, 0.0f, 2.0f, true}},
        {"[0,5] skew=0.4 step=1", {0.0f, 5.0f, 0.0f, 1.0f, 0.4f, false}},
        {"[0,8] skew=3 step=1", {0.0f, 8.0f, 0.0f, 1.0f, 3.0f, false}},
        {"[0,12] skew=0.7 step=0", {0.0f, 12.0f, 0.0f, 0.0f, 0.7f, false}},
    };
}

} // namespace

TEST_CASE("AAX mirror converges for a discrete parameter that declares no step",
          "[aax][view-bridge]") {
    // A legal, plausible declaration: the author states the semantic kind and
    // lets the framework infer the stepping, exactly as CLAP/VST3/AU allow.
    // constrain_stored_value() then applies an implicit step of 1 that the AAX
    // taper knows nothing about, so the two sides hold structurally different
    // values and never compare equal as raw reals.
    const state::ParamRange range{-24.0f, 24.0f, 0.0f, 0.0f, 1.0f, false};
    const auto probe = run_to_quiescence(range, state::ParamKind::Integer, 0.325);

    INFO("writes=" << probe.writes << " aax=" << probe.aax_value
                   << " store=" << probe.store_value);
    CHECK(probe.converged);
    CHECK(probe.refused == 0);  // converged on the rule, not on the fuse
    // The store quantizes to the integer; AAX holds the same value in its own
    // coordinates, one taper round trip away. Both are -8 to the user.
    CHECK(probe.store_value == -8.0f);
    CHECK(taper_real_to_normalized(range, probe.aax_value) ==
          taper_real_to_normalized(range, -8.0f));
}

TEST_CASE("AAX mirror converges across every declarable range and kind",
          "[aax][view-bridge]") {
    const state::ParamKind kinds[] = {state::ParamKind::Continuous,
                                      state::ParamKind::Integer,
                                      state::ParamKind::Toggle,
                                      state::ParamKind::Enum};

    int cases = 0;
    int runaways = 0;
    int fuse_carried = 0;
    long long worst = 0;
    for (const auto& named : convergence_ranges()) {
        for (const auto kind : kinds) {
            for (int i = 0; i <= 200; ++i) {
                const auto probe =
                    run_to_quiescence(named.range, kind, i / 200.0);
                ++cases;
                worst = std::max(worst, probe.writes);
                if (!probe.converged) {
                    ++runaways;
                    UNSCOPED_INFO("runaway: " << named.name << " kind=" << static_cast<int>(kind)
                                              << " n0=" << (i / 200.0)
                                              << " aax=" << probe.aax_value
                                              << " store=" << probe.store_value);
                }
                if (probe.refused > 0) {
                    ++fuse_carried;
                    UNSCOPED_INFO("fuse-carried: " << named.name
                                                   << " kind=" << static_cast<int>(kind)
                                                   << " n0=" << (i / 200.0));
                }
            }
        }
    }

    INFO("cases=" << cases << " worst_corrective_writes=" << worst
                  << " fuse_carried=" << fuse_carried);
    CHECK(cases == 16884);
    CHECK(runaways == 0);
    // The rule has to do this on its own. The fuse bounds a runaway at
    // kCorrectionFuseLimit writes, so it would hide a broken rule here as
    // "converged" — a case that reaches the fuse is a case the rule got wrong.
    CHECK(fuse_carried == 0);
    // And a correct rule needs exactly one write to close the gap.
    CHECK(worst <= 1);
}

TEST_CASE("AAX mirror fuse bounds a host that never converges",
          "[aax][view-bridge]") {
    // No real host should do this, and after the fix above no range does either.
    // The fuse is what makes an unknown host quirk fail safe instead of wedging
    // Pro Tools with an unbounded touch/post/release stream.
    const auto definition = make_definition();
    state::StateStore store;
    add_definition_parameters(store);

    int writes = 0;
    ParameterMirror mirror{
        definition,
        // A host stuck one epsilon away from whatever it is told, forever.
        [](const char*, float& out) { out = -1.0f; return true; },
        [&writes](const char*, float) { ++writes; return true; }};

    for (int i = 0; i < 100; ++i) {
        mirror.on_store_value_changed(kGain, 0.5f);
    }

    CHECK(writes == static_cast<int>(ParameterMirror::kCorrectionFuseLimit));
    CHECK(mirror.is_fused(kGain));
    CHECK(mirror.refused_count() == 100 - ParameterMirror::kCorrectionFuseLimit);
}

TEST_CASE("AAX mirror fuse re-arms on the next distinct value",
          "[aax][view-bridge]") {
    // A blown fuse must not deafen the parameter for the rest of the session:
    // the user's next knob move is a new value and deserves a write.
    const auto definition = make_definition();
    state::StateStore store;
    add_definition_parameters(store);

    int writes = 0;
    ParameterMirror mirror{
        definition,
        [](const char*, float& out) { out = -1.0f; return true; },
        [&writes](const char*, float) { ++writes; return true; }};

    for (int i = 0; i < 100; ++i) {
        mirror.on_store_value_changed(kGain, 0.5f);
    }
    REQUIRE(mirror.is_fused(kGain));

    CHECK(mirror.on_store_value_changed(kGain, 0.25f));  // a new value writes
    CHECK_FALSE(mirror.is_fused(kGain));
    CHECK(writes == static_cast<int>(ParameterMirror::kCorrectionFuseLimit) + 1);
}

TEST_CASE("AAX mirror fuse never trips on a knob drag", "[aax][view-bridge]") {
    // Every value in a drag is distinct, so the fuse must stay armed throughout
    // however long the drag runs.
    MirrorFixture f;
    for (int i = 1; i <= 200; ++i) {
        f.store.set_value(kGain, static_cast<float>(i) / 200.0f);
        f.aax.settle();
    }
    CHECK(f.mirror.refused_count() == 0);
    CHECK_FALSE(f.mirror.is_fused(kGain));
    CHECK(f.store.get_value(kGain) == 1.0f);
}

TEST_CASE("AAX mirror reports a read it could not perform", "[aax][view-bridge]") {
    // AAX_IParameter::GetValueAsFloat answers false without writing through the
    // out-pointer for a non-float parameter. A reader that swallowed that would
    // leave `out` at zero and the mirror would "correct" the host to zero.
    const auto definition = make_definition();
    int writes = 0;
    ParameterMirror mirror{
        definition,
        [](const char*, float&) { return false; },  // value unreadable, out untouched
        [&writes](const char*, float) { ++writes; return true; }};

    CHECK_FALSE(mirror.on_store_value_changed(kGain, 0.75f));
    CHECK(writes == 0);  // no write on a fabricated value
    CHECK(mirror.suppressed_count() == 0);
    CHECK(mirror.refused_count() == 0);
}

TEST_CASE("AAX mirror reports a write the manager refused", "[aax][view-bridge]") {
    const auto definition = make_definition();
    ParameterMirror mirror{
        definition,
        [](const char*, float& out) { out = 0.0f; return true; },
        [](const char*, float) { return false; }};  // AAX_IParameter refused

    CHECK_FALSE(mirror.on_store_value_changed(kGain, 0.75f));
}

TEST_CASE("AAX host dimensions stay in logical points on Retina hosts",
          "[aax][view-bridge][resize]") {
    CHECK(aax_host_dimension(990, 1.0f) == 990);
    CHECK(aax_host_dimension(990, 2.0f) == 990);
    CHECK(aax_host_dimension(645, 2.0f) == 645);
    CHECK(aax_host_dimension(101, 1.5f) == 101);
    CHECK(aax_host_dimension(0, 2.0f) == 0);
    CHECK(aax_host_dimension(990, 0.0f) == 990);
}

// ── Contract editors ─────────────────────────────────────────────────────────
//
// ContractEditorShell, the AAX GUI's path for a processor with an editor
// factory, driven the way PulpAaxEffectGUI drives it, over a fake data model.
// The editor records every contract call.

#include "support/recording_contract_editor.hpp"

namespace {

using pulp::test_support::contract_config;
using pulp::test_support::contract_log;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

class FactoryProcessor final : public pulp::format::Processor {
  public:
    explicit FactoryProcessor(pulp::format::EditorFactory* factory) : factory_(factory) {}
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "AAX contract editor";
        d.bundle_id = "dev.pulp.test.aax-contract-editor";
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = 1;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    pulp::format::EditorFactory* editor_factory() override {
        return factory_;
    }

  private:
    pulp::format::EditorFactory* factory_;
};

/// The data model the AAX GUI reaches through GetEffectParameters().
class FakeModel final : public pulp::format::aax::EditorHost {
  public:
    explicit FakeModel(pulp::format::EditorFactory* factory) : processor(factory) {
        processor.set_state_store(&store);
        processor.define_parameters(store);
    }
    pulp::format::Processor* editor_processor() override {
        return has_processor ? &processor : nullptr;
    }
    pulp::state::StateStore* editor_store() override {
        return has_processor ? &store : nullptr;
    }
    std::optional<pulp::format::ViewSize> editor_view_size() override {
        return processor.view_size();
    }
    const pulp::format::aax::PluginDefinition& editor_definition() const override {
        return definition;
    }
    void release_editor_gestures() override {
        ++gesture_releases;
    }
    void set_editor_invalidation_handler(std::function<void()> handler) override {
        invalidation = std::move(handler);
    }
    // The model's own call: it copies the handler first, as aax_runtime does.
    void invalidate_processor() {
        if (auto handler = invalidation)
            handler();
    }

    pulp::state::StateStore store;
    FactoryProcessor processor;
    pulp::format::aax::PluginDefinition definition;
    bool has_processor = true;
    int gesture_releases = 0;
    std::function<void()> invalidation;
};

int g_aax_fake_view = 0;
pulp::format::NativeParent aax_parent() {
    return {.kind = pulp::format::NativeParentKind::cocoa_view, .handle = &g_aax_fake_view};
}

} // namespace

TEST_CASE("AAX builds a factory editor before its window, sizes from it and attaches it",
          "[aax][editor-host][aax-contract]") {
    using pulp::format::aax::ContractEditorShell;
    reset_contract_recording();
    contract_config() = {};
    RecordingContractFactory factory;
    FakeModel model(&factory);
    REQUIRE(ContractEditorShell::applies(model));

    std::vector<std::pair<uint32_t, uint32_t>> published;
    ContractEditorShell shell;
    // CreateViewContents: built, not attached, already able to size the host.
    REQUIRE(shell.create(model, [&published](uint32_t w, uint32_t h) {
        published.emplace_back(w, h);
        return true;
    }));
    REQUIRE(shell.built());
    REQUIRE_FALSE(shell.attached());
    REQUIRE_FALSE(contract_log().saw("attach:cocoa_view"));
    auto plan = shell.plan();
    REQUIRE(plan);
    CHECK(plan->width == 400);
    CHECK(plan->height == 300);
    CHECK(plan->min_width == 200);
    CHECK(plan->min_height == 150);
    CHECK(plan->resizable);

    // CreateViewContainer.
    REQUIRE(shell.attach(aax_parent()));
    CHECK(contract_log().saw("attach:cocoa_view"));
    CHECK(contract_log().parent_handle == &g_aax_fake_view);

    // An editor resize request becomes SetViewSize, and the editor takes the
    // size the host accepted.
    REQUIRE(contract_log().context->request_resize({640, 480, 1.0}));
    REQUIRE(published.size() == 1);
    CHECK(published[0] == std::pair<uint32_t, uint32_t>{640, 480});
    CHECK(contract_log().saw("set_size:640x480"));
    plan = shell.plan();
    CHECK(plan->width == 640);
    CHECK(plan->height == 480);

    // A native frame change from the host lands inside the editor's hints.
    REQUIRE(shell.host_resized(100, 100));
    CHECK(contract_log().saw("set_size:200x150"));

    // DeleteViewContainer: gestures released, editor detached and kept.
    shell.detach();
    CHECK(contract_log().saw("detach"));
    CHECK(model.gesture_releases == 1);
    CHECK(shell.built());
    CHECK(contract_log().live == 1);
    // A second container reattaches the same editor.
    REQUIRE(shell.attach(aax_parent()));

    // GUI destroyed.
    shell.close();
    CHECK_FALSE(shell.built());
    CHECK(contract_log().live == 0);
    CHECK_FALSE(contract_log().saw("invalidate"));
    CHECK(model.gesture_releases == 2);
    CHECK_FALSE(model.invalidation);
}

TEST_CASE("AAX follows the host container when it is resized",
          "[aax][editor-host][aax-contract][resize]") {
    using pulp::format::aax::ContractEditorShell;
    reset_contract_recording();
    contract_config() = {};
    RecordingContractFactory factory;
    FakeModel model(&factory);
    std::vector<std::pair<uint32_t, uint32_t>> published;
    ContractEditorShell shell;
    REQUIRE(shell.create(model, [&published](uint32_t w, uint32_t h) {
        published.emplace_back(w, h);
        return true;
    }));

    // The container's size as the host last set it.
    uint32_t parent_width = 400;
    uint32_t parent_height = 300;
    int reads = 0;
    void* read_from = nullptr;
    const ContractEditorShell::ParentSizeReader reader = [&](void* parent, uint32_t& w,
                                                             uint32_t& h) {
        ++reads;
        read_from = parent;
        w = parent_width;
        h = parent_height;
        return true;
    };

    // Nothing to follow before the container exists.
    CHECK_FALSE(shell.follow_parent_frame(reader));
    CHECK(reads == 0);

    REQUIRE(shell.attach(aax_parent()));
    // A container at the editor's own size changes nothing.
    CHECK_FALSE(shell.follow_parent_frame(reader));
    CHECK(read_from == &g_aax_fake_view);
    CHECK_FALSE(contract_log().saw("set_size:400x300"));

    // The host or user drags the window larger: the editor takes the new size.
    parent_width = 640;
    parent_height = 480;
    CHECK(shell.follow_parent_frame(reader));
    CHECK(contract_log().saw("set_size:640x480"));
    CHECK(shell.plan()->width == 640);
    CHECK(shell.plan()->height == 480);

    // The same container size is not applied twice.
    contract_log().calls.clear();
    CHECK_FALSE(shell.follow_parent_frame(reader));
    CHECK_FALSE(contract_log().saw("set_size:640x480"));

    // The editor's own request is echoed back by the host at the size it
    // already took, and is not applied a second time.
    REQUIRE(contract_log().context->request_resize({560, 420, 1.0}));
    CHECK(published.back() == std::pair<uint32_t, uint32_t>{560, 420});
    contract_log().calls.clear();
    parent_width = 560;
    parent_height = 420;
    CHECK_FALSE(shell.follow_parent_frame(reader));
    CHECK_FALSE(contract_log().saw("set_size:560x420"));

    // A container that cannot be read, or no reader, leaves the editor alone.
    const ContractEditorShell::ParentSizeReader unreadable = [](void*, uint32_t&, uint32_t&) {
        return false;
    };
    CHECK_FALSE(shell.follow_parent_frame(unreadable));
    CHECK_FALSE(shell.follow_parent_frame({}));

    // A container the host took away is no longer followed.
    shell.detach();
    parent_width = 800;
    parent_height = 600;
    reads = 0;
    CHECK_FALSE(shell.follow_parent_frame(reader));
    CHECK(reads == 0);

    // A new container is followed from scratch.
    REQUIRE(shell.attach(aax_parent()));
    contract_log().calls.clear();
    CHECK(shell.follow_parent_frame(reader));
    CHECK(contract_log().saw("set_size:800x600"));
    shell.close();
}

TEST_CASE("AAX exposes the requested size during host publication and restores rejected sizes",
          "[aax][editor-host][aax-contract][resize]") {
    for (const bool fixed : {false, true}) {
        for (const bool accept : {false, true}) {
            reset_contract_recording();
            contract_config() = {};
            contract_config().fixed_size = fixed;
            RecordingContractFactory factory;
            FakeModel model(&factory);
            ContractEditorShell shell;
            int publications = 0;
            REQUIRE(shell.create(model, [&](uint32_t width, uint32_t height) {
                ++publications;
                const auto during = shell.plan();
                REQUIRE(during);
                CHECK(during->width == width);
                CHECK(during->height == height);
                // Only the host-facing answer is staged before acceptance.
                CHECK(shell.session()->size().width == 400);
                CHECK(shell.session()->size().height == 300);
                return accept;
            }));
            REQUIRE(shell.attach(aax_parent()));
            CHECK(contract_log().context->request_resize({640, 480, 1.0}) == accept);
            CHECK(publications == 1);
            const auto after = shell.plan();
            REQUIRE(after);
            CHECK(after->width == (accept ? 640 : 400));
            CHECK(after->height == (accept ? 480 : 300));
            CHECK(shell.session()->size().width == after->width);
            CHECK(shell.session()->size().height == after->height);
        }
    }
    contract_config() = {};
}

TEST_CASE("AAX closes a factory editor before the model deletes its processor",
          "[aax][editor-host][aax-contract]") {
    using pulp::format::aax::ContractEditorShell;
    reset_contract_recording();
    contract_config() = {};
    RecordingContractFactory factory;
    FakeModel model(&factory);
    ContractEditorShell shell;
    REQUIRE(shell.create(model, {}));
    REQUIRE(shell.attach(aax_parent()));
    REQUIRE(model.invalidation);

    model.invalidate_processor();
    CHECK_FALSE(shell.built());
    CHECK(contract_log().saw("detach"));
    CHECK(contract_log().live == 0);
    CHECK_FALSE(model.invalidation);
}

TEST_CASE("AAX does not build or size a factory editor without a parent size reader",
          "[aax][editor-host][aax-contract][negative]") {
    reset_contract_recording();
    contract_config() = {};
    RecordingContractFactory factory;
    FakeModel model(&factory);
    ContractEditorShell shell;
    REQUIRE(ContractEditorShell::applies(model));

    CHECK_FALSE(shell.create(model, {}, false));
    CHECK(contract_log().calls.empty());
    CHECK_FALSE(shell.built());
    CHECK_FALSE(shell.plan());
    CHECK(contract_log().live == 0);
    CHECK_FALSE(model.invalidation);
    CHECK_FALSE(shell.attach(aax_parent()));

    // The same factory is usable when the adapter can follow parent resizes.
    REQUIRE(shell.create(model, {}, true));
    CHECK(shell.built());
    CHECK(shell.plan());
    CHECK(contract_log().live == 1);
}

TEST_CASE("AAX leaves the create_view() path to a processor without an editor factory",
          "[aax][editor-host][aax-contract][negative]") {
    using pulp::format::aax::ContractEditorShell;
    reset_contract_recording();
    contract_config() = {};
    FakeModel no_factory(nullptr);
    ContractEditorShell shell;
    CHECK_FALSE(ContractEditorShell::applies(no_factory));
    CHECK_FALSE(shell.create(no_factory, {}));
    CHECK_FALSE(shell.plan());

    RecordingContractFactory factory;
    FakeModel no_processor(&factory);
    no_processor.has_processor = false;
    CHECK_FALSE(ContractEditorShell::applies(no_processor));
    CHECK_FALSE(shell.create(no_processor, {}));
}

TEST_CASE("AAX keeps a factory editor unattached when the host's view refuses it",
          "[aax][editor-host][aax-contract][negative]") {
    using pulp::format::aax::ContractEditorShell;
    reset_contract_recording();
    contract_config() = {};
    contract_config().attach_succeeds = false;
    RecordingContractFactory factory;
    FakeModel model(&factory);
    int publishes = 0;
    ContractEditorShell shell;
    REQUIRE(shell.create(model, [&publishes](uint32_t, uint32_t) {
        ++publishes;
        return true;
    }));
    CHECK_FALSE(shell.attach(aax_parent()));
    CHECK_FALSE(shell.attached());
    // An unattached editor never resizes the host's window.
    CHECK_FALSE(contract_log().context->request_resize({640, 480, 1.0}));
    CHECK(publishes == 0);
    CHECK_FALSE(shell.host_resized(640, 480));
    shell.close();
    CHECK(contract_log().live == 0);
    contract_config() = {};
}

// ── A Vellum editor in the AAX shell ─────────────────────────────────────────
//
// This source built a second time against Pulp's Vellum bridge: the same
// shell over a pulp::vellum editor and its Vellum PluginViewHost, attached
// offscreen to another Vellum host's native view. Only these cases run in
// that executable.

#if defined(PULP_TEST_AAX_VELLUM)

namespace {

struct AaxRootLog {
    int opens = 0;
    int closes = 0;
    ::vellum::view::View* root = nullptr;
};
AaxRootLog g_aax_root;

class AaxRootView final : public ::vellum::view::View {
  public:
    explicit AaxRootView(pulp::vellum::EditorEnv&) {
        g_aax_root.root = this;
    }
    ~AaxRootView() override {
        g_aax_root.root = nullptr;
    }
    pulp::format::ViewSize editor_view_size() const {
        return {.preferred_width = 400,
                .preferred_height = 300,
                .min_width = 200,
                .min_height = 150,
                .max_width = 800,
                .max_height = 600,
                .aspect_ratio = 4.0 / 3.0};
    }
    void editor_opened() {
        ++g_aax_root.opens;
    }
    void editor_closing() {
        ++g_aax_root.closes;
    }
};

struct AaxHostFactoryReset {
    ~AaxHostFactoryReset() {
        ::vellum::view::PluginViewHost::clear_factory();
    }
};

} // namespace

TEST_CASE("AAX opens, sizes and closes a Vellum editor", "[aax][vellum][editor-host]") {
#if !defined(__APPLE__)
    SKIP("Vellum's SDK ships its GPU plug-in view host on macOS only");
#endif
    using pulp::format::aax::ContractEditorShell;
    g_aax_root = {};
    FakeModel model(pulp::vellum::editor_factory<AaxRootView>());
    ContractEditorShell shell;
    REQUIRE(shell.create(model, [](uint32_t, uint32_t) { return true; }));
    REQUIRE(g_aax_root.root != nullptr);
    auto* host = g_aax_root.root->plugin_view_host();
    REQUIRE(host != nullptr);
    // GetViewSize before any window: the root's declared size.
    const auto plan = shell.plan();
    REQUIRE(plan);
    CHECK(plan->width == 400);
    CHECK(plan->height == 300);
    CHECK(g_aax_root.opens == 0);

    ::vellum::view::View parent_root;
    auto parent = ::vellum::view::PluginViewHost::create(
        parent_root, ::vellum::view::PluginViewHost::Size{800, 600});
    REQUIRE(parent != nullptr);
    REQUIRE(shell.attach(
        {.kind = pulp::format::NativeParentKind::cocoa_view, .handle = parent->native_handle()}));
    CHECK(g_aax_root.opens == 1);
    CHECK(host->is_attached());

    REQUIRE(shell.host_resized(640, 480));
    CHECK(host->get_size().width == 640);
    CHECK(host->get_size().height == 480);

    shell.detach();
    CHECK(g_aax_root.closes == 1);
    CHECK_FALSE(host->is_attached());
    shell.close();
    CHECK(g_aax_root.root == nullptr);
}

TEST_CASE("AAX keeps a Vellum editor closed when its view host cannot be created",
          "[aax][vellum][editor-host][negative]") {
    using pulp::format::aax::ContractEditorShell;
    AaxHostFactoryReset reset;
    ::vellum::view::PluginViewHost::set_factory(
        [](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&)
            -> std::unique_ptr<::vellum::view::PluginViewHost> { return nullptr; });
    g_aax_root = {};
    FakeModel model(pulp::vellum::editor_factory<AaxRootView>());
    ContractEditorShell shell;
    REQUIRE(shell.create(model, {}));
    int parent_stand_in = 0;
    CHECK_FALSE(shell.attach(
        {.kind = pulp::format::NativeParentKind::cocoa_view, .handle = &parent_stand_in}));
    CHECK(g_aax_root.opens == 0);
    shell.close();
    CHECK(g_aax_root.closes == 0);
}

TEST_CASE("AAX offers the contract editor only where the host container size is readable",
          "[aax][vellum][resize]") {
    using namespace pulp::format::aax;
    std::uint32_t w = 0, h = 0;
    int stand_in = 0;
    // A platform without a container size reader cannot follow a host resize,
    // so the flag must be false there and true wherever the reader works.
#if defined(__APPLE__)
    CHECK(kAaxParentSizeReadable);
#else
    CHECK_FALSE(kAaxParentSizeReadable);
    CHECK_FALSE(aax_native_parent_size(&stand_in, w, h));
#endif
    (void)w;
    (void)h;
    (void)stand_in;
}

#endif // PULP_TEST_AAX_VELLUM
