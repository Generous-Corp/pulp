// Host-side VST3 separated-controller state sync (pulp::host::detail).
//
// Vst3Slot lives in an anonymous namespace and is not test-reachable, so the
// state serialize/restore/sync logic is exercised here through the free
// functions in vst3_state_sync.hpp with fake IComponent / IEditController.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/detail/vst3_state_sync.hpp>

#include <public.sdk/source/vst/vstcomponent.h>
#include <public.sdk/source/vst/vsteditcontroller.h>

#include <cstdint>
#include <vector>

using namespace Steinberg;
using pulp::host::detail::VectorStream;
using pulp::host::detail::vst3_restore_state;
using pulp::host::detail::vst3_serialize_state;

namespace {

// Read an IBStream to completion (seek to end for size, then read from 0).
std::vector<uint8_t> read_all(IBStream* s) {
    int64 end = 0;
    s->seek(0, IBStream::kIBSeekEnd, &end);
    s->seek(0, IBStream::kIBSeekSet, nullptr);
    std::vector<uint8_t> out((size_t)(end > 0 ? end : 0));
    if (end > 0) {
        int32 got = 0;
        s->read(out.data(), (int32)end, &got);
        out.resize((size_t)got);
    }
    return out;
}

// Fake processor: hands back an opaque state blob and records restores.
class FakeComponent : public Vst::Component {
public:
    std::vector<uint8_t> state{'C', 'O', 'M', 'P'};
    std::vector<uint8_t> last_set;
    int set_calls = 0;

    tresult PLUGIN_API getState(IBStream* s) SMTG_OVERRIDE {
        return s->write(state.data(), (int32)state.size(), nullptr);
    }
    tresult PLUGIN_API setState(IBStream* s) SMTG_OVERRIDE {
        last_set = read_all(s);
        state = last_set;
        ++set_calls;
        return kResultOk;
    }
};

// Fake separated edit controller: records setComponentState / setState / getState.
class FakeController : public Vst::EditController {
public:
    std::vector<uint8_t> state{'C', 'T', 'R', 'L'};
    std::vector<uint8_t> last_set_component;
    std::vector<uint8_t> last_set_state;
    int set_component_calls = 0;
    int set_state_calls = 0;

    tresult PLUGIN_API getState(IBStream* s) SMTG_OVERRIDE {
        return s->write(state.data(), (int32)state.size(), nullptr);
    }
    tresult PLUGIN_API setComponentState(IBStream* s) SMTG_OVERRIDE {
        last_set_component = read_all(s);
        ++set_component_calls;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* s) SMTG_OVERRIDE {
        last_set_state = read_all(s);
        state = last_set_state;
        ++set_state_calls;
        return kResultOk;
    }
};

std::vector<uint8_t> bytes(const char* s) {
    return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s));
}

}  // namespace

TEST_CASE("VST3 separated state round-trips both component and controller",
          "[host][vst3][state][issue-8]") {
    FakeComponent comp;
    FakeController ctrl;

    auto blob = vst3_serialize_state(&comp, &ctrl, /*separated=*/true);
    REQUIRE_FALSE(blob.empty());
    // Versioned container: starts with the PV3S magic.
    REQUIRE(blob.size() >= 4);
    REQUIRE(blob[0] == 'P');
    REQUIRE(blob[1] == 'V');
    REQUIRE(blob[2] == '3');
    REQUIRE(blob[3] == 'S');

    REQUIRE(vst3_restore_state(blob, &comp, &ctrl, /*separated=*/true));
    // Processor state was applied.
    REQUIRE(comp.set_calls == 1);
    REQUIRE(comp.last_set == bytes("COMP"));
    // Controller was synced with the *component* state via setComponentState...
    REQUIRE(ctrl.set_component_calls == 1);
    REQUIRE(ctrl.last_set_component == bytes("COMP"));
    // ...and its own state restored via setState.
    REQUIRE(ctrl.set_state_calls == 1);
    REQUIRE(ctrl.last_set_state == bytes("CTRL"));
}

TEST_CASE("VST3 restore of a legacy (magic-less) blob still syncs the controller",
          "[host][vst3][state][issue-8]") {
    FakeComponent comp;
    FakeController ctrl;

    // A blob written by pre-seam Pulp: raw component state, no container.
    auto legacy = bytes("RAWCOMPONENTSTATE");

    REQUIRE(vst3_restore_state(legacy, &comp, &ctrl, /*separated=*/true));
    REQUIRE(comp.set_calls == 1);
    REQUIRE(comp.last_set == legacy);
    // The controller is still synced with the component state...
    REQUIRE(ctrl.set_component_calls == 1);
    REQUIRE(ctrl.last_set_component == legacy);
    // ...but there is no separate controller section, so setState is not called.
    REQUIRE(ctrl.set_state_calls == 0);
}

TEST_CASE("VST3 combined plugin does not touch a controller",
          "[host][vst3][state][issue-8]") {
    FakeComponent comp;
    FakeController ctrl;  // present, but we pass separated=false

    auto blob = vst3_serialize_state(&comp, &ctrl, /*separated=*/false);
    REQUIRE_FALSE(blob.empty());

    REQUIRE(vst3_restore_state(blob, &comp, &ctrl, /*separated=*/false));
    REQUIRE(comp.set_calls == 1);
    REQUIRE(comp.last_set == bytes("COMP"));
    // A combined plugin's controller state is not written or synced separately.
    REQUIRE(ctrl.set_component_calls == 0);
    REQUIRE(ctrl.set_state_calls == 0);
}

TEST_CASE("VST3 restore rejects truncated containers without reading OOB",
          "[host][vst3][state][issue-8]") {
    FakeComponent comp;
    FakeController ctrl;
    const auto full = vst3_serialize_state(&comp, &ctrl, /*separated=*/true);
    REQUIRE(full.size() > 24);  // magic4 + ver4 + clen8 + COMP4 + tlen8 + CTRL4

    SECTION("chopped controller tail") {
        auto blob = full;
        blob.resize(blob.size() - 2);
        REQUIRE_FALSE(vst3_restore_state(blob, &comp, &ctrl, /*separated=*/true));
        REQUIRE(comp.set_calls == 0);
    }
    SECTION("truncated right after component_len (body missing) — underflow guard") {
        // magic(4) + version(4) + component_len(8) = 16 bytes, but the header
        // declares a 4-byte component that isn't present. A naive size check
        // would underflow and read out of bounds.
        auto blob = full;
        blob.resize(16);
        REQUIRE_FALSE(vst3_restore_state(blob, &comp, &ctrl, /*separated=*/true));
        REQUIRE(comp.set_calls == 0);
    }
    SECTION("component present but controller_len field missing") {
        // Keep through the 4-byte component (offset 20), drop the 8-byte
        // controller_len that must follow.
        auto blob = full;
        blob.resize(20 + 3);  // component ends at 20; only 3 of 8 len bytes
        REQUIRE_FALSE(vst3_restore_state(blob, &comp, &ctrl, /*separated=*/true));
        REQUIRE(comp.set_calls == 0);
    }
}
