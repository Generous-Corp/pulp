#pragma once

// A VST3 host's UI run loop (Steinberg::Linux::IRunLoop, reached through the
// host's IPlugFrame) as a platform::HostRunLoop: the host's timers and file
// descriptor watches carry the platform layer's main-thread work, with no
// thread of the plug-in's own.
//
// Linux only. Main (UI) thread only, except `on_ui_thread`.

#include <pulp/platform/main_thread.hpp>

#include <pluginterfaces/gui/iplugview.h>

#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace pulp::format::vst3 {

class HostRunLoop final : public platform::HostRunLoop {
  public:
    /// Take a reference on `run_loop` (released on destruction).
    explicit HostRunLoop(Steinberg::Linux::IRunLoop* run_loop);
    ~HostRunLoop() override;

    /// The run loop of `frame` (an IPlugFrame the host gave a view), or null
    /// when the host offers none.
    static std::unique_ptr<HostRunLoop> from_frame(Steinberg::IPlugFrame* frame);

    bool on_ui_thread() const override;
    std::uint64_t add_timer(std::chrono::milliseconds period, std::function<void()> tick) override;
    void remove_timer(std::uint64_t id) override;
    std::uint64_t add_fd(int fd, std::function<void()> readable) override;
    void remove_fd(std::uint64_t id) override;

  private:
    class Timer;
    class Watch;

    // Unregistered handlers are kept until no host callback is running, so one
    // that removes itself from inside its own callback is not freed under it.
    void collect();

    Steinberg::Linux::IRunLoop* loop_ = nullptr;
    std::thread::id ui_ = std::this_thread::get_id();
    std::uint64_t next_id_ = 1;
    int callbacks_running_ = 0;
    std::map<std::uint64_t, std::unique_ptr<Timer>> timers_;
    std::map<std::uint64_t, std::unique_ptr<Watch>> watches_;
    std::vector<std::unique_ptr<Timer>> retired_timers_;
    std::vector<std::unique_ptr<Watch>> retired_watches_;
};

} // namespace pulp::format::vst3
