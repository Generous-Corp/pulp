#pragma once

// VST3 separated-model connection points.
//
// A VST3 plug-in may ship as one object implementing both IComponent and
// IEditController ("combined"), or as two objects that only reach each other
// through IConnectionPoint ("separated"). The host owns that link: it queries
// both halves for their connection point and connects each to the other.
//
// Leaving a separated plug-in unconnected is not a cosmetic gap. Everything a
// plug-in cannot express as a parameter — preset banks, meter feeds, editor
// handshakes — travels as IMessage over that link, so an unconnected plug-in
// opens an editor that can never talk to its own processor. The messages
// themselves are allocated by the host's IHostApplication::createInstance, so a
// host that connects the points must also supply a real host application.
//
// Telling the two shapes apart is itself a trap, so it lives here too — see
// vst3_is_separated.
//
// The two halves are joined DIRECTLY, with no marshalling proxy in between (the
// SDK's optional ConnectionProxy). A message a plug-in sends from its audio
// thread therefore arrives on the other half from that same thread, which is
// what the VST3 validator and simple hosts do. A plug-in that needs its
// controller notified on the UI thread would need a proxy here — nothing in
// Pulp provides one yet, so do not assume the notify() side is main-thread.
//
// This is the testable seam for that wiring: plugin_slot_vst3.cpp calls it,
// test_vst3_connection.cpp drives it with fake IComponent / IEditController.
//
// This header pulls in VST3 SDK interfaces, so it must only be included from
// translation units compiled with the SDK on the include path (PULP_HAS_VST3).

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>

namespace pulp::host::detail {

/// Whether two interface pointers name the same underlying object.
///
/// COM identity is defined by the FUnknown query, not by pointer arithmetic.
/// Comparing `static_cast<FUnknown*>` of an IComponent* against the same cast
/// of an IEditController* does NOT work: a combined plug-in inherits the two
/// interfaces separately, so each carries its own FUnknown base subobject at a
/// different address, and the comparison calls every combined plug-in
/// "separated". Querying FUnknown from both sides returns one canonical
/// pointer, which is the only reliable answer.
inline bool vst3_same_object(Steinberg::FUnknown* a, Steinberg::FUnknown* b) {
    if (a == b) return a != nullptr;
    if (!a || !b) return false;
    Steinberg::FUnknown* canonical_a = nullptr;
    Steinberg::FUnknown* canonical_b = nullptr;
    if (a->queryInterface(Steinberg::FUnknown::iid, (void**)&canonical_a) != Steinberg::kResultOk
        || !canonical_a) {
        return false;
    }
    if (b->queryInterface(Steinberg::FUnknown::iid, (void**)&canonical_b) != Steinberg::kResultOk
        || !canonical_b) {
        canonical_a->release();
        return false;
    }
    const bool same = (canonical_a == canonical_b);
    canonical_a->release();
    canonical_b->release();
    return same;
}

/// Whether this plug-in uses VST3's separated model — two distinct objects —
/// rather than one object implementing both halves. Callers key teardown,
/// state serialization, and connection-point wiring off this: a combined
/// plug-in mistaken for a separated one gets terminate() twice and its state
/// stored twice.
inline bool vst3_is_separated(Steinberg::Vst::IComponent* component,
                              Steinberg::Vst::IEditController* controller) {
    if (!component || !controller) return false;
    return !vst3_same_object(component, controller);
}

/// Why a connect attempt ended the way it did. Only `Connected` means messages
/// can flow; the rest are all legal outcomes a host must tolerate.
enum class Vst3ConnectResult {
    Connected,           ///< Both halves accepted the other.
    NotSeparated,        ///< Combined plug-in (or a null half) — nothing to connect.
    NoConnectionPoints,  ///< One or both halves expose no IConnectionPoint.
    ComponentRefused,    ///< The component rejected its controller.
    ControllerRefused,   ///< The controller rejected its component.
};

/// Owned connection-point references plus whether they are currently joined.
/// Always hand a live instance to vst3_disconnect_component_controller() — it
/// is what releases the references, including on the failure paths.
struct Vst3Connection {
    Steinberg::Vst::IConnectionPoint* component = nullptr;
    Steinberg::Vst::IConnectionPoint* controller = nullptr;
    Vst3ConnectResult result = Vst3ConnectResult::NotSeparated;

    bool connected() const { return result == Vst3ConnectResult::Connected; }
};

/// Join a separated plug-in's two halves. A combined plug-in (component and
/// controller are the same object) must never be connected to itself, so it
/// returns `NotSeparated` without touching either pointer.
inline Vst3Connection vst3_connect_component_controller(
    Steinberg::Vst::IComponent* component,
    Steinberg::Vst::IEditController* controller) {
    Vst3Connection conn;
    if (!vst3_is_separated(component, controller)) return conn;

    if (component->queryInterface(Steinberg::Vst::IConnectionPoint::iid,
                                  (void**)&conn.component) != Steinberg::kResultOk) {
        conn.component = nullptr;
    }
    if (controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid,
                                   (void**)&conn.controller) != Steinberg::kResultOk) {
        conn.controller = nullptr;
    }
    if (!conn.component || !conn.controller) {
        conn.result = Vst3ConnectResult::NoConnectionPoints;
        return conn;
    }

    if (conn.component->connect(conn.controller) != Steinberg::kResultOk) {
        conn.result = Vst3ConnectResult::ComponentRefused;
        return conn;
    }
    if (conn.controller->connect(conn.component) != Steinberg::kResultOk) {
        // Unwind the half that did accept so the plug-in is not left holding a
        // one-way link the host believes is absent.
        conn.component->disconnect(conn.controller);
        conn.result = Vst3ConnectResult::ControllerRefused;
        return conn;
    }
    conn.result = Vst3ConnectResult::Connected;
    return conn;
}

/// Break the link (if it was made) and drop both references. Idempotent, and
/// safe on a connection returned from any failure path.
inline void vst3_disconnect_component_controller(Vst3Connection& conn) {
    if (conn.connected() && conn.component && conn.controller) {
        conn.component->disconnect(conn.controller);
        conn.controller->disconnect(conn.component);
    }
    conn.result = Vst3ConnectResult::NotSeparated;
    if (conn.component) {
        conn.component->release();
        conn.component = nullptr;
    }
    if (conn.controller) {
        conn.controller->release();
        conn.controller = nullptr;
    }
}

}  // namespace pulp::host::detail
