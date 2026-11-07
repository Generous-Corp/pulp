// Platform-independent tests for the port-list diffing and polling notifier
// that give the Windows multimedia MIDI backend hotplug notifications.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/port_change_watcher.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace pulp::midi;

namespace {

MidiPortInfo in_port(std::string id, std::string name) {
    MidiPortInfo p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.is_input = true;
    return p;
}

MidiPortInfo out_port(std::string id, std::string name) {
    MidiPortInfo p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.is_output = true;
    return p;
}

}  // namespace

TEST_CASE("diff_port_lists reports added and removed ports", "[midi][hotplug]") {
    const auto d = diff_port_lists({in_port("0", "Pad"), in_port("1", "Keys")},
                                   {in_port("0", "Pad"), in_port("2", "Mixer")});
    REQUIRE(d.removed.size() == 1);
    REQUIRE(d.removed[0].id == "1");
    REQUIRE(d.added.size() == 1);
    REQUIRE(d.added[0].name == "Mixer");
    REQUIRE_FALSE(d.empty());
}

TEST_CASE("diff_port_lists is empty for identical lists in any order", "[midi][hotplug]") {
    const auto d = diff_port_lists({in_port("0", "A"), in_port("1", "B")},
                                   {in_port("1", "B"), in_port("0", "A")});
    REQUIRE(d.empty());
    REQUIRE(diff_port_lists({}, {}).empty());
}

TEST_CASE("diff_port_lists treats positional ids that shift as changes", "[midi][hotplug]") {
    // Removing the first of two ports renumbers the second.
    const auto d = diff_port_lists({in_port("0", "Pad"), in_port("1", "Keys")},
                                   {in_port("0", "Keys")});
    REQUIRE(d.removed.size() == 2);
    REQUIRE(d.added.size() == 1);
    REQUIRE(d.added[0].id == "0");
    REQUIRE(d.added[0].name == "Keys");
}

TEST_CASE("diff_port_lists distinguishes direction and duplicate names", "[midi][hotplug]") {
    // An input and an output with the same id and name are different ports.
    auto d = diff_port_lists({in_port("0", "Box"), out_port("0", "Box")}, {in_port("0", "Box")});
    REQUIRE(d.removed.size() == 1);
    REQUIRE(d.removed[0].is_output);
    REQUIRE(d.added.empty());

    // Two identical devices: losing one is one removal, not zero.
    d = diff_port_lists({in_port("0", "Pad"), in_port("0", "Pad")}, {in_port("0", "Pad")});
    REQUIRE(d.removed.size() == 1);
    REQUIRE(d.added.empty());
}

TEST_CASE("PortListTracker's first update is a baseline", "[midi][hotplug]") {
    PortListTracker t;
    REQUIRE(t.update({in_port("0", "A")}).empty());
    REQUIRE(t.update({in_port("0", "A")}).empty());
    const auto d = t.update({in_port("0", "A"), in_port("1", "B")});
    REQUIRE(d.added.size() == 1);
    REQUIRE(t.update({in_port("0", "A")}).removed.size() == 1);
}

TEST_CASE("PortChangePoller notifies on change and stays quiet otherwise", "[midi][hotplug]") {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<MidiPortInfo> ports{in_port("0", "A")};
    std::vector<PortListDiff> seen;

    PortChangePoller poller;
    poller.start(
        [&] {
            std::lock_guard<std::mutex> lk(mu);
            return ports;
        },
        [&](const PortListDiff& d) {
            std::lock_guard<std::mutex> lk(mu);
            seen.push_back(d);
            cv.notify_all();
        },
        std::chrono::milliseconds(5));

    // No change: nothing reported after several polls.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard<std::mutex> lk(mu);
        REQUIRE(seen.empty());
        ports.push_back(in_port("1", "B"));
    }
    {
        std::unique_lock<std::mutex> lk(mu);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return seen.size() == 1; }));
        REQUIRE(seen[0].added.size() == 1);
        REQUIRE(seen[0].added[0].id == "1");
        ports.clear();
    }
    {
        std::unique_lock<std::mutex> lk(mu);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return seen.size() == 2; }));
        REQUIRE(seen[1].removed.size() == 2);
    }
    poller.stop();
    REQUIRE_FALSE(poller.running());
}

TEST_CASE("PortChangePoller takes its baseline before the thread starts", "[midi][hotplug]") {
    std::atomic<int> notified{0};
    std::atomic<bool> changed{false};
    PortChangePoller poller;
    // The list is already different by the time the first timer tick runs.
    poller.start(
        [&] {
            if (changed) return std::vector<MidiPortInfo>{in_port("0", "A"), in_port("1", "B")};
            return std::vector<MidiPortInfo>{in_port("0", "A")};
        },
        [&](const PortListDiff&) { ++notified; },
        std::chrono::milliseconds(5));
    changed = true;
    for (int i = 0; i < 500 && notified == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(notified >= 1);
}

TEST_CASE("PortChangePoller stop prevents later callbacks and handles restarts", "[midi][hotplug]") {
    std::atomic<int> notified{0};
    std::atomic<int> generation{0};
    PortChangePoller poller;
    auto enumerate = [&] {
        std::vector<MidiPortInfo> v;
        for (int i = 0; i <= generation; ++i) v.push_back(in_port(std::to_string(i), "P"));
        return v;
    };
    poller.start(enumerate, [&](const PortListDiff&) { ++notified; }, std::chrono::milliseconds(5));
    poller.stop();
    const int after_stop = notified;
    ++generation;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    REQUIRE(notified == after_stop);

    // Restart re-baselines against the current list.
    poller.start(enumerate, [&](const PortListDiff&) { ++notified; }, std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    REQUIRE(notified == after_stop);
    ++generation;
    for (int i = 0; i < 500 && notified == after_stop; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(notified > after_stop);
}

TEST_CASE("PortChangePoller tolerates stop() from its own callback", "[midi][hotplug]") {
    std::atomic<int> notified{0};
    std::atomic<bool> grow{false};
    PortChangePoller poller;
    poller.start(
        [&] {
            std::vector<MidiPortInfo> v{in_port("0", "A")};
            if (grow) v.push_back(in_port("1", "B"));
            return v;
        },
        [&](const PortListDiff&) { ++notified; poller.stop(); },
        std::chrono::milliseconds(5));
    grow = true;
    for (int i = 0; i < 500 && notified == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(notified == 1);
    poller.stop();  // joins the wound-down thread
    REQUIRE_FALSE(poller.running());
}
