#pragma once

// A CLAP host's timer-support and posix-fd-support extensions as a
// platform::HostRunLoop: the host's timers and descriptor watches carry the
// platform layer's main-thread work, with no thread of the plug-in's own.
//
// The host calls back through the plug-in's own extensions, so the adapter
// forwards `on_timer` and `on_fd` here. Main thread only, except `on_ui_thread`.

#include <pulp/platform/main_thread.hpp>

#include <clap/clap.h>
#include <clap/ext/posix-fd-support.h>
#include <clap/ext/timer-support.h>

#include <map>
#include <thread>

namespace pulp::format::clap_adapter {

class ClapHostRunLoop final : public platform::HostRunLoop {
  public:
    /// Look the extensions up on `host`. Timers are required for the loop to
    /// be usable; descriptor watches are optional (the platform layer polls on
    /// a timer without them).
    explicit ClapHostRunLoop(const clap_host_t* host);
    ~ClapHostRunLoop() override;

    /// Whether the host offers timers.
    bool usable() const {
        return timer_ != nullptr;
    }

    /// Whether the host watches descriptors; without it the platform layer
    /// polls on a timer.
    bool watches_fds() const {
        return fd_ != nullptr;
    }

    /// The host's timer `id` fired: run its callback. Unknown ids are ignored.
    void on_timer(clap_id id);
    /// The host reports `fd` readable or in error: run its callback.
    void on_fd(int fd, clap_posix_fd_flags_t flags);

    bool on_ui_thread() const override;
    std::uint64_t add_timer(std::chrono::milliseconds period, std::function<void()> tick) override;
    void remove_timer(std::uint64_t id) override;
    std::uint64_t add_fd(int fd, std::function<void()> readable) override;
    void remove_fd(std::uint64_t id) override;

  private:
    const clap_host_t* host_ = nullptr;
    const clap_host_timer_support_t* timer_ = nullptr;
    const clap_host_posix_fd_support_t* fd_ = nullptr;
    std::thread::id ui_ = std::this_thread::get_id();
    std::map<clap_id, std::function<void()>> timers_;
    std::map<int, std::function<void()>> watches_;
};

} // namespace pulp::format::clap_adapter
