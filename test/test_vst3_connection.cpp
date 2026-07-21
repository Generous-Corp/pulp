// Host-side VST3 separated-model connection points
// (pulp::host::detail::vst3_connection.hpp) and the load-time controller state
// push (vst3_push_component_state).
//
// Vst3Slot is anonymous-namespace and needs a real .vst3 bundle to reach, so
// the wiring lives in free-function seams the slot calls and this test drives
// with fake IComponent / IEditController halves.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/detail/vst3_connection.hpp>
#include <pulp/host/detail/vst3_state_sync.hpp>

#include <public.sdk/source/vst/vsteditcontroller.h>
#include <public.sdk/source/vst/vstsinglecomponenteffect.h>

#include <cstdint>
#include <vector>

using namespace Steinberg;
using pulp::host::detail::Vst3ConnectResult;
using pulp::host::detail::vst3_connect_component_controller;
using pulp::host::detail::vst3_disconnect_component_controller;
using pulp::host::detail::vst3_push_component_state;

namespace {

// One half of a separated plug-in. Reference counting is recorded rather than
// enforced so the test can assert the seam balances its queryInterface +1s
// without the object dying underneath it.
class FakeConnectionPoint : public Vst::IConnectionPoint {
public:
    bool refuse_connect = false;
    bool expose = true;  // whether the owning half hands this out at all

    Vst::IConnectionPoint* connected_to = nullptr;
    int connect_calls = 0;
    int disconnect_calls = 0;
    int ref_count = 0;

    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) SMTG_OVERRIDE {
        ++connect_calls;
        if (refuse_connect) return kResultFalse;
        connected_to = other;
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint* /*other*/) SMTG_OVERRIDE {
        ++disconnect_calls;
        connected_to = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage*) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, Vst::IConnectionPoint::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IConnectionPoint*>(this);
            addRef();
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return (uint32)++ref_count; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return (uint32)--ref_count; }
};

// Minimal IComponent that owns a connection point and a state blob.
class FakeComponent : public Vst::IComponent {
public:
    FakeConnectionPoint point;
    std::vector<uint8_t> state{'s', 't', '8'};
    bool state_fails = false;

    tresult PLUGIN_API getState(IBStream* stream) SMTG_OVERRIDE {
        if (state_fails || !stream) return kResultFalse;
        int32 written = 0;
        return stream->write(state.data(), (int32)state.size(), &written);
    }
    tresult PLUGIN_API setState(IBStream*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API initialize(FUnknown*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API terminate() SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API getControllerClassId(TUID) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(Vst::IoMode) SMTG_OVERRIDE { return kNotImplemented; }
    int32 PLUGIN_API getBusCount(Vst::MediaType, Vst::BusDirection) SMTG_OVERRIDE { return 0; }
    tresult PLUGIN_API getBusInfo(Vst::MediaType, Vst::BusDirection, int32,
                                  Vst::BusInfo&) SMTG_OVERRIDE {
        return kResultFalse;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) SMTG_OVERRIDE {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus(Vst::MediaType, Vst::BusDirection, int32,
                                   TBool) SMTG_OVERRIDE {
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (point.expose
            && FUnknownPrivate::iidEqual(iid, Vst::IConnectionPoint::iid)) {
            return point.queryInterface(iid, obj);
        }
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponent::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IComponent*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }
};

// Controller half. Inherits the SDK EditController so only the pieces the seam
// touches need overriding.
class FakeController : public Vst::EditController {
public:
    FakeConnectionPoint point;
    std::vector<uint8_t> component_state_seen;
    int set_component_state_calls = 0;

    tresult PLUGIN_API setComponentState(IBStream* stream) SMTG_OVERRIDE {
        ++set_component_state_calls;
        component_state_seen.clear();
        if (!stream) return kResultFalse;
        uint8_t byte = 0;
        int32 read = 0;
        while (stream->read(&byte, 1, &read) == kResultOk && read == 1) {
            component_state_seen.push_back(byte);
        }
        return kResultOk;
    }

    // The vst3-sdk library carries EditControllerEx1 (via
    // vstsinglecomponenteffect.cpp) but not EditController's own state methods,
    // so define them here rather than pulling in a whole extra SDK source.
    tresult PLUGIN_API getState(IBStream*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (point.expose
            && FUnknownPrivate::iidEqual(iid, Vst::IConnectionPoint::iid)) {
            return point.queryInterface(iid, obj);
        }
        return Vst::EditController::queryInterface(iid, obj);
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }
};

// A non-conformant half: it refuses the FUnknown query that decides identity.
class RefusesFUnknown : public Vst::IComponent {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponent::iid)) {
            *obj = static_cast<Vst::IComponent*>(this);
            return kResultTrue;
        }
        *obj = nullptr;   // including FUnknown::iid
        return kNoInterface;
    }
    tresult PLUGIN_API getState(IBStream*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API initialize(FUnknown*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API terminate() SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API getControllerClassId(TUID) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(Vst::IoMode) SMTG_OVERRIDE { return kNotImplemented; }
    int32 PLUGIN_API getBusCount(Vst::MediaType, Vst::BusDirection) SMTG_OVERRIDE { return 0; }
    tresult PLUGIN_API getBusInfo(Vst::MediaType, Vst::BusDirection, int32,
                                  Vst::BusInfo&) SMTG_OVERRIDE {
        return kResultFalse;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) SMTG_OVERRIDE {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus(Vst::MediaType, Vst::BusDirection, int32,
                                   TBool) SMTG_OVERRIDE {
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool) SMTG_OVERRIDE { return kResultOk; }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }
};

}  // namespace

TEST_CASE("VST3 undeterminable identity fails toward the leak, not the crash",
          "[host][vst3][connection]") {
    RefusesFUnknown component;   // cannot answer the query identity depends on
    FakeController controller;

    // The two wrong answers are not equally bad: calling one object "two"
    // terminates it twice (undefined behavior inside the plug-in), while
    // calling two objects "one" only skips a terminate. Prefer the leak.
    REQUIRE(pulp::host::detail::vst3_same_object(
        static_cast<Vst::IComponent*>(&component),
        static_cast<Vst::IEditController*>(&controller)));
    REQUIRE_FALSE(pulp::host::detail::vst3_is_separated(&component, &controller));

    // And nothing is connected on a guess.
    auto conn = vst3_connect_component_controller(&component, &controller);
    REQUIRE(conn.result == Vst3ConnectResult::NotSeparated);
    REQUIRE(controller.point.connect_calls == 0);
}

TEST_CASE("VST3 combined plug-in is recognized as one object",
          "[host][vst3][connection]") {
    // The SDK's own combined base class — one object implementing IComponent
    // and IEditController — is the authoritative shape to test against.
    Vst::SingleComponentEffect combined;
    auto* as_component = static_cast<Vst::IComponent*>(&combined);
    auto* as_controller = static_cast<Vst::IEditController*>(&combined);

    // The trap this guards: the two interfaces are inherited separately, so
    // each carries its own FUnknown base subobject at a different address and a
    // cast-and-compare calls this object "separated".
    REQUIRE(static_cast<FUnknown*>(as_component) != static_cast<FUnknown*>(as_controller));

    REQUIRE(pulp::host::detail::vst3_same_object(as_component, as_controller));
    REQUIRE_FALSE(pulp::host::detail::vst3_is_separated(as_component, as_controller));
}

TEST_CASE("VST3 separated halves are recognized as two objects",
          "[host][vst3][connection]") {
    FakeComponent component;
    FakeController controller;
    auto* controller_iface = static_cast<Vst::IEditController*>(&controller);
    REQUIRE(pulp::host::detail::vst3_is_separated(&component, controller_iface));
    REQUIRE_FALSE(pulp::host::detail::vst3_same_object(&component, controller_iface));
    // A null half is not a plug-in with two halves.
    REQUIRE_FALSE(pulp::host::detail::vst3_is_separated(nullptr, controller_iface));
    REQUIRE_FALSE(pulp::host::detail::vst3_is_separated(&component, nullptr));
    REQUIRE_FALSE(pulp::host::detail::vst3_same_object(nullptr, nullptr));
}

TEST_CASE("VST3 separated halves are connected in both directions",
          "[host][vst3][connection]") {
    FakeComponent component;
    FakeController controller;

    auto conn = vst3_connect_component_controller(&component, &controller);

    REQUIRE(conn.result == Vst3ConnectResult::Connected);
    REQUIRE(conn.connected());
    // Each half must hold the other: a one-way link leaves messages travelling
    // in only one direction.
    REQUIRE(component.point.connected_to == static_cast<Vst::IConnectionPoint*>(&controller.point));
    REQUIRE(controller.point.connected_to == static_cast<Vst::IConnectionPoint*>(&component.point));

    vst3_disconnect_component_controller(conn);
    REQUIRE(component.point.disconnect_calls == 1);
    REQUIRE(controller.point.disconnect_calls == 1);
    // The queryInterface +1 on each half is given back.
    REQUIRE(component.point.ref_count == 0);
    REQUIRE(controller.point.ref_count == 0);
    REQUIRE(conn.component == nullptr);
    REQUIRE(conn.controller == nullptr);
}

TEST_CASE("VST3 combined plug-in is never connected to itself",
          "[host][vst3][connection]") {
    // Connecting a combined plug-in to itself would have it notify() its own
    // messages straight back. SingleComponentEffect does expose an
    // IConnectionPoint, so nothing but the identity check stops that.
    Vst::SingleComponentEffect combined;

    auto conn = vst3_connect_component_controller(
        static_cast<Vst::IComponent*>(&combined),
        static_cast<Vst::IEditController*>(&combined));

    REQUIRE(conn.result == Vst3ConnectResult::NotSeparated);
    REQUIRE(conn.component == nullptr);
    REQUIRE(conn.controller == nullptr);
}

TEST_CASE("VST3 connect tolerates a half with no connection point",
          "[host][vst3][connection]") {
    FakeComponent component;
    component.point.expose = false;  // this half has nothing to say
    FakeController controller;

    auto conn = vst3_connect_component_controller(&component, &controller);

    REQUIRE(conn.result == Vst3ConnectResult::NoConnectionPoints);
    REQUIRE(controller.point.connect_calls == 0);
    // The reference taken off the half that DID expose one is still released.
    vst3_disconnect_component_controller(conn);
    REQUIRE(controller.point.ref_count == 0);
    REQUIRE(conn.controller == nullptr);
}

TEST_CASE("VST3 connect unwinds the accepted half when the other refuses",
          "[host][vst3][connection]") {
    FakeComponent component;
    FakeController controller;
    controller.point.refuse_connect = true;

    auto conn = vst3_connect_component_controller(&component, &controller);

    REQUIRE(conn.result == Vst3ConnectResult::ControllerRefused);
    REQUIRE_FALSE(conn.connected());
    // The component accepted, so it is holding a link the host is abandoning;
    // leaving it would have the plug-in believe it is half-connected.
    REQUIRE(component.point.connect_calls == 1);
    REQUIRE(component.point.disconnect_calls == 1);
    REQUIRE(component.point.connected_to == nullptr);

    vst3_disconnect_component_controller(conn);
    // Already unwound — disconnect must not be issued a second time.
    REQUIRE(component.point.disconnect_calls == 1);
    REQUIRE(component.point.ref_count == 0);
    REQUIRE(controller.point.ref_count == 0);
}

TEST_CASE("VST3 connect reports a component that refuses its controller",
          "[host][vst3][connection]") {
    FakeComponent component;
    FakeController controller;
    component.point.refuse_connect = true;

    auto conn = vst3_connect_component_controller(&component, &controller);

    REQUIRE(conn.result == Vst3ConnectResult::ComponentRefused);
    REQUIRE(controller.point.connect_calls == 0);
    vst3_disconnect_component_controller(conn);
    REQUIRE(component.point.ref_count == 0);
    REQUIRE(controller.point.ref_count == 0);
}

TEST_CASE("VST3 disconnect is idempotent and null-safe",
          "[host][vst3][connection]") {
    FakeComponent component;
    FakeController controller;
    auto conn = vst3_connect_component_controller(&component, &controller);
    vst3_disconnect_component_controller(conn);
    vst3_disconnect_component_controller(conn);
    REQUIRE(component.point.disconnect_calls == 1);
    REQUIRE(component.point.ref_count == 0);

    auto empty = vst3_connect_component_controller(nullptr, &controller);
    REQUIRE(empty.result == Vst3ConnectResult::NotSeparated);
    vst3_disconnect_component_controller(empty);
}

TEST_CASE("VST3 load pushes the component state into a separated controller",
          "[host][vst3][connection]") {
    FakeComponent component;
    component.state = {0xDE, 0xAD, 0xBE, 0xEF};
    FakeController controller;

    REQUIRE(vst3_push_component_state(&component, &controller));

    // Without this the controller — and so the editor about to open — shows its
    // own defaults instead of what the processor will render.
    REQUIRE(controller.set_component_state_calls == 1);
    REQUIRE(controller.component_state_seen == component.state);
}

TEST_CASE("VST3 component state push reports a component with no state",
          "[host][vst3][connection]") {
    FakeComponent component;
    component.state_fails = true;
    FakeController controller;

    REQUIRE_FALSE(vst3_push_component_state(&component, &controller));
    REQUIRE(controller.set_component_state_calls == 0);
    REQUIRE_FALSE(vst3_push_component_state(nullptr, &controller));
    REQUIRE_FALSE(vst3_push_component_state(&component, nullptr));
}
