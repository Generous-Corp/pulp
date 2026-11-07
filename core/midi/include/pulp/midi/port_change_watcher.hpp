#pragma once

/// @file port_change_watcher.hpp
/// Port-list diffing and a polling change notifier.
///
/// Some MIDI backends (the classic Windows multimedia API) have no plug or
/// unplug notification of their own. They can still report hotplug by
/// enumerating on a timer and comparing snapshots. The comparison and the
/// timer thread live here, free of any platform header, so the logic is the
/// same everywhere and testable without hardware.

#include <pulp/midi/device.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::midi {

/// Ports that appeared and ports that went away between two snapshots.
struct PortListDiff {
    std::vector<MidiPortInfo> added;
    std::vector<MidiPortInfo> removed;
    bool empty() const {
        return added.empty() && removed.empty();
    }
};

namespace detail {
inline bool same_port(const MidiPortInfo& a, const MidiPortInfo& b) {
    return a.id == b.id && a.name == b.name && a.is_input == b.is_input &&
           a.is_output == b.is_output;
}
inline bool contains_port(const std::vector<MidiPortInfo>& list, const MidiPortInfo& p) {
    for (const auto& q : list) {
        if (same_port(p, q))
            return true;
    }
    return false;
}
} // namespace detail

/// Compare two snapshots. A port is identified by id, name and direction
/// together. Backends that number ports by position (so removing one shifts
/// the ids of the rest) therefore report the shifted ports as removed and
/// added, which is the truth for anyone holding the old ids. Duplicate
/// entries are matched one for one.
inline PortListDiff diff_port_lists(const std::vector<MidiPortInfo>& before,
                                    const std::vector<MidiPortInfo>& after) {
    PortListDiff d;
    std::vector<bool> matched(after.size(), false);
    for (const auto& b : before) {
        bool found = false;
        for (std::size_t i = 0; i < after.size(); ++i) {
            if (!matched[i] && detail::same_port(b, after[i])) {
                matched[i] = true;
                found = true;
                break;
            }
        }
        if (!found)
            d.removed.push_back(b);
    }
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (!matched[i])
            d.added.push_back(after[i]);
    }
    return d;
}

/// Remembers the last snapshot and reports what changed since.
class PortListTracker {
  public:
    /// Record `current` and return the difference from the previous snapshot.
    /// The first call establishes the baseline and reports no change.
    PortListDiff update(std::vector<MidiPortInfo> current) {
        PortListDiff d;
        if (have_baseline_)
            d = diff_port_lists(last_, current);
        last_ = std::move(current);
        have_baseline_ = true;
        return d;
    }

  private:
    std::vector<MidiPortInfo> last_;
    bool have_baseline_ = false;
};

/// Calls `on_change` from a background thread whenever the combined input and
/// output list changes. `enumerate` returns the current ports and runs on that
/// thread. Destruction (or `stop()`) joins the thread, so no callback runs
/// afterwards. `stop()` may be called from `on_change`, in which case the
/// thread winds down and is joined by the next `stop()` or the destructor.
class PortChangePoller {
  public:
    using Enumerate = std::function<std::vector<MidiPortInfo>()>;
    using OnChange = std::function<void(const PortListDiff&)>;

    PortChangePoller() = default;
    PortChangePoller(const PortChangePoller&) = delete;
    PortChangePoller& operator=(const PortChangePoller&) = delete;
    ~PortChangePoller() {
        stop();
    }

    /// Start polling. The baseline is taken on the calling thread before the
    /// worker starts, so a change right after `start()` is never missed.
    /// Restarts if already running.
    void start(Enumerate enumerate, OnChange on_change,
               std::chrono::milliseconds interval = std::chrono::milliseconds(1000)) {
        // A change handler that re-registers from the worker thread cannot
        // restart its own thread; ignore the request.
        if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id())
            return;
        stop();
        tracker_ = PortListTracker{};
        tracker_.update(enumerate());
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = false;
        }
        worker_ = std::thread(
            [this, enumerate = std::move(enumerate), on_change = std::move(on_change), interval] {
                std::unique_lock<std::mutex> lk(mu_);
                while (!stopping_) {
                    cv_.wait_for(lk, interval, [this] { return stopping_; });
                    if (stopping_)
                        break;
                    lk.unlock();
                    const auto diff = tracker_.update(enumerate());
                    if (!diff.empty() && on_change)
                        on_change(diff);
                    lk.lock();
                }
            });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        // Called from inside `on_change`: the loop sees the flag and exits on
        // its own; the next stop() or the destructor joins it.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
            worker_.join();
    }

    bool running() const {
        return worker_.joinable();
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread worker_;
    PortListTracker tracker_;
};

} // namespace pulp::midi
