// test_host_parameter_edit.cpp — editor parameter-write provenance (WAH-5).
//
// The bug: the VST3 adapter decided a StateStore write came from the editor by
// checking whether a gesture was OPEN. That is an inference, and it is wrong in
// a case users hit constantly — host automation arriving while the user holds a
// knob. The automation landed in the store, "is a gesture open?" said yes, and
// the adapter echoed the host's own value back at it through performEdit().
//
// The second defect was quieter: the callback read `get_normalized(id)` TWICE
// per reported edit — once for performEdit, once for setParamNormalized — so a
// write landing between the two reads reported one logical change as two
// different values.
//
// These tests interleave host automation with editor edits during an open
// gesture and assert the exact begin/value/end stream, because ordering is what
// hosts actually latch on for touch/latch automation and undo grouping.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/host_parameter_edit.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::format;
using pulp::state::ParamID;
using pulp::state::StateStore;

namespace {

/// Records the exact protocol stream so ORDER is assertable, not just counts.
struct EditLog {
    struct Entry {
        enum class Kind { begin, value, end } kind;
        ParamID id;
        float normalized = 0.0f;
    };
    std::vector<Entry> entries;

    HostParameterEditBridge::Callbacks callbacks() {
        return {
            [this](ParamID id) {
                entries.push_back({Entry::Kind::begin, id, 0.0f});
            },
            [this](ParamID id, float n) {
                entries.push_back({Entry::Kind::value, id, n});
            },
            [this](ParamID id) {
                entries.push_back({Entry::Kind::end, id, 0.0f});
            },
        };
    }

    /// Compact rendering ("B V(0.5) E") so a failure message shows the stream.
    std::string stream() const {
        std::string s;
        for (const auto& e : entries) {
            if (!s.empty()) s += ' ';
            switch (e.kind) {
                case Entry::Kind::begin: s += 'B'; break;
                case Entry::Kind::end: s += 'E'; break;
                case Entry::Kind::value: s += 'V'; break;
            }
        }
        return s;
    }

    size_t count(Entry::Kind kind) const {
        size_t n = 0;
        for (const auto& e : entries)
            if (e.kind == kind) ++n;
        return n;
    }
};

constexpr ParamID kGain = 1;

void add_unit_param(StateStore& store, ParamID id, const char* name) {
    pulp::state::ParamInfo info;
    info.id = id;
    info.name = name;
    info.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(info);
}

}  // namespace

TEST_CASE("an editor gesture produces begin, value, end in order",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    store.begin_gesture(kGain);
    store.set_normalized(kGain, 0.75f);
    store.end_gesture(kGain);

    // The final value is reported BEFORE the gesture closes, so a host that
    // latches on end records where the control actually landed.
    REQUIRE(log.stream() == "B V V E");
    REQUIRE(log.entries.front().kind == EditLog::Entry::Kind::begin);
    REQUIRE(log.entries.back().kind == EditLog::Entry::Kind::end);
}

TEST_CASE("every reported edit carries one coherent normalized snapshot",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    store.begin_gesture(kGain);
    store.set_normalized(kGain, 0.25f);
    store.end_gesture(kGain);

    for (const auto& e : log.entries) {
        if (e.kind != EditLog::Entry::Kind::value) continue;
        // The old code read get_normalized() twice per edit and handed the two
        // results to two different host calls.
        REQUIRE(e.normalized == 0.25f);
    }
}

TEST_CASE("host automation during an open gesture is NOT echoed back",
          "[host-parameter-edit][wah-5]") {
    // The exact interleaving that broke: the user is holding a knob (gesture
    // open) when the host plays back automation into the same parameter.
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    store.begin_gesture(kGain);
    store.set_normalized(kGain, 0.6f);  // the user's edit — reportable
    const size_t after_user_edit = log.entries.size();

    {
        const ScopedHostParameterWrite host_write;
        store.set_normalized(kGain, 0.1f);  // the host's own value
        store.set_normalized(kGain, 0.2f);
    }

    REQUIRE(log.entries.size() == after_user_edit);  // nothing added
    store.end_gesture(kGain);
}

TEST_CASE("a host write outside any gesture is also silent",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    const ScopedHostParameterWrite host_write;
    store.set_normalized(kGain, 0.9f);

    REQUIRE(log.entries.empty());
}

TEST_CASE("processor-originated writes are not reported as editor edits",
          "[host-parameter-edit][wah-5]") {
    // Output parameters a plug-in writes during process() reach the host
    // through the adapter's own output-parameter path; reporting them here
    // would double-report them.
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    const ScopedProcessorParameterWrite processor_write;
    store.set_normalized(kGain, 0.33f);

    REQUIRE(log.entries.empty());
}

TEST_CASE("origin scopes nest and only lift at the outermost exit",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    {
        const ScopedHostParameterWrite outer;
        {
            const ScopedHostParameterWrite inner;
            store.set_normalized(kGain, 0.2f);
        }
        // Still inside `outer` — a naive boolean flag would have re-enabled
        // reporting here and leaked the rest of the host's writes.
        store.set_normalized(kGain, 0.3f);
        REQUIRE(log.entries.empty());
    }

    store.set_normalized(kGain, 0.4f);
    REQUIRE(log.count(EditLog::Entry::Kind::value) == 1);
}

TEST_CASE("editor edits resume immediately after a host write",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    store.begin_gesture(kGain);
    {
        const ScopedHostParameterWrite host_write;
        store.set_normalized(kGain, 0.1f);
    }
    store.set_normalized(kGain, 0.8f);  // user moved the knob again
    store.end_gesture(kGain);

    // begin, the user's value, the end-of-gesture final value, end. The host's
    // write contributed nothing.
    REQUIRE(log.stream() == "B V V E");
    REQUIRE(log.entries[1].normalized == 0.8f);
}

TEST_CASE("a write from a non-editor thread is not reported",
          "[host-parameter-edit][wah-5]") {
    // Host automation is applied on the AUDIO thread. The thread check alone
    // must reject it, with no origin scope required — an adapter that forgets
    // to mark a scope still cannot echo audio-thread automation.
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    std::thread audio([&] { store.set_normalized(kGain, 0.45f); });
    audio.join();

    REQUIRE(log.entries.empty());
    // And the store DID take the value — suppression is about reporting, not
    // about dropping the host's automation.
    REQUIRE(store.get_normalized(kGain) == 0.45f);
}

TEST_CASE("would_report reflects the calling thread and origin scope",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    REQUIRE(bridge.would_report());
    {
        const ScopedHostParameterWrite host_write;
        REQUIRE_FALSE(bridge.would_report());
    }
    REQUIRE(bridge.would_report());

    bool other_thread_reports = true;
    std::thread t([&] { other_thread_reports = bridge.would_report(); });
    t.join();
    REQUIRE_FALSE(other_thread_reports);
}

TEST_CASE("an origin scope on one thread does not silence another",
          "[host-parameter-edit][wah-5]") {
    // The scopes are thread_local: a host write in flight on the audio thread
    // must not suppress a genuine editor edit happening at the same moment.
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    std::thread other([] {
        const ScopedHostParameterWrite host_write;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    // Meanwhile, on the editor thread:
    store.set_normalized(kGain, 0.55f);
    other.join();

    REQUIRE(log.count(EditLog::Entry::Kind::value) == 1);
}

TEST_CASE("destroying the bridge stops all reporting",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    EditLog log;
    {
        HostParameterEditBridge bridge(store, log.callbacks());
        store.set_normalized(kGain, 0.6f);
        REQUIRE(log.count(EditLog::Entry::Kind::value) == 1);
    }

    store.begin_gesture(kGain);
    store.set_normalized(kGain, 0.7f);
    store.end_gesture(kGain);

    // The editor is gone; a late store write must not reach a dead adapter.
    REQUIRE(log.count(EditLog::Entry::Kind::value) == 1);
    REQUIRE(log.count(EditLog::Entry::Kind::begin) == 0);
}

TEST_CASE("gestures on different parameters keep their own streams",
          "[host-parameter-edit][wah-5]") {
    StateStore store;
    add_unit_param(store, kGain, "Gain");
    constexpr ParamID kPan = 2;
    add_unit_param(store, kPan, "Pan");

    EditLog log;
    HostParameterEditBridge bridge(store, log.callbacks());

    store.begin_gesture(kGain);
    store.begin_gesture(kPan);
    store.set_normalized(kGain, 0.2f);
    store.set_normalized(kPan, 0.8f);
    store.end_gesture(kPan);
    store.end_gesture(kGain);

    // Each parameter's begin must be matched by its own end — an unbalanced
    // pair strands the host's automation record open forever.
    size_t gain_begin = 0, gain_end = 0, pan_begin = 0, pan_end = 0;
    for (const auto& e : log.entries) {
        if (e.kind == EditLog::Entry::Kind::begin)
            (e.id == kGain ? gain_begin : pan_begin)++;
        else if (e.kind == EditLog::Entry::Kind::end)
            (e.id == kGain ? gain_end : pan_end)++;
    }
    REQUIRE(gain_begin == 1);
    REQUIRE(gain_end == 1);
    REQUIRE(pan_begin == 1);
    REQUIRE(pan_end == 1);
}
