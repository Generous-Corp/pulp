#pragma once

#ifdef _WIN32

#include "accessibility_win_fragment_topology.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pulp::view {

struct UiaSession;
class PulpFragmentProvider;

/// Concrete owner of the Windows UIA fragment publication transaction.
/// Provider construction, reader exclusion, reentrant retirement, and deferred
/// disconnect are one backend-private protocol rather than callback policy.
class UiaFragmentLifecycle {
public:
    class ReadLease {
    public:
        explicit ReadLease(UiaFragmentLifecycle& lifecycle);
        explicit operator bool() const noexcept { return available_; }
        const std::vector<PulpFragmentProvider*>& providers() const noexcept {
            return lifecycle_.active_;
        }
        int first_child() const noexcept { return lifecycle_.first_child_; }
        int last_child() const noexcept { return lifecycle_.last_child_; }

    private:
        UiaFragmentLifecycle& lifecycle_;
        std::unique_lock<std::mutex> lock_;
        bool available_ = false;
    };

    ReadLease read() { return ReadLease(*this); }
    void rebuild(UiaSession* session, View* root);
    void release();
    void drain();
    void abandon_retired();
    void retain_retired(std::shared_ptr<void> owner);
    std::size_t active_size() const noexcept { return active_.size(); }
    std::size_t retired_size() const noexcept;

private:
    void publish(std::vector<PulpFragmentProvider*>&& providers,
                 int first_child, int last_child);
    void retire_active();
    void retire_unpublished(std::vector<PulpFragmentProvider*> providers);

    std::atomic<bool> available_{false};
    std::mutex reader_mutex_;
    std::vector<PulpFragmentProvider*> active_;
    std::vector<PulpFragmentProvider*> retired_;
    std::vector<PulpFragmentProvider*> draining_;
    bool draining_active_ = false;
    int first_child_ = -1;
    int last_child_ = -1;
    std::uint64_t publication_generation_ = 0;
};

} // namespace pulp::view

#endif
