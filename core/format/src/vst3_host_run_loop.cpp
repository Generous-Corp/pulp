#include "vst3_host_run_loop.hpp"

#include <algorithm>
#include <utility>

namespace pulp::format::vst3 {

using namespace Steinberg;

// The host holds these by raw pointer between registerTimer and
// unregisterTimer, so reference counting is not theirs to drive.
class HostRunLoop::Timer final : public Linux::ITimerHandler {
  public:
    Timer(HostRunLoop& owner, std::function<void()> tick) : owner_(owner), tick_(std::move(tick)) {}

    void PLUGIN_API onTimer() override {
        // Copy first: the tick may remove this timer, and with it `tick_`.
        auto tick = tick_;
        ++owner_.callbacks_running_;
        if (tick)
            tick();
        --owner_.callbacks_running_;
        owner_.collect();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (obj == nullptr)
            return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler*>(this);
            return kResultTrue;
        }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override {
        return 1;
    }
    uint32 PLUGIN_API release() override {
        return 1;
    }

  private:
    HostRunLoop& owner_;
    std::function<void()> tick_;
};

class HostRunLoop::Watch final : public Linux::IEventHandler {
  public:
    Watch(HostRunLoop& owner, std::function<void()> readable)
        : owner_(owner), readable_(std::move(readable)) {}

    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) override {
        auto readable = readable_;
        ++owner_.callbacks_running_;
        if (readable)
            readable();
        --owner_.callbacks_running_;
        owner_.collect();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (obj == nullptr)
            return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(iid, Linux::IEventHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::IEventHandler*>(this);
            return kResultTrue;
        }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override {
        return 1;
    }
    uint32 PLUGIN_API release() override {
        return 1;
    }

  private:
    HostRunLoop& owner_;
    std::function<void()> readable_;
};

HostRunLoop::HostRunLoop(Linux::IRunLoop* run_loop) : loop_(run_loop) {
    if (loop_ != nullptr)
        loop_->addRef();
}

HostRunLoop::~HostRunLoop() {
    // The platform layer removes everything it added before it lets go of us;
    // anything left is withdrawn here so the host keeps no pointer into us.
    for (auto& entry : timers_)
        loop_->unregisterTimer(entry.second.get());
    for (auto& entry : watches_)
        loop_->unregisterEventHandler(entry.second.get());
    timers_.clear();
    watches_.clear();
    if (loop_ != nullptr)
        loop_->release();
}

std::unique_ptr<HostRunLoop> HostRunLoop::from_frame(IPlugFrame* frame) {
    if (frame == nullptr)
        return nullptr;
    void* queried = nullptr;
    if (frame->queryInterface(Linux::IRunLoop::iid, &queried) != kResultTrue || queried == nullptr)
        return nullptr;
    auto* run_loop = static_cast<Linux::IRunLoop*>(queried);
    auto result = std::make_unique<HostRunLoop>(run_loop);
    run_loop->release(); // queryInterface added one; the object holds its own
    return result;
}

bool HostRunLoop::on_ui_thread() const {
    return std::this_thread::get_id() == ui_;
}

void HostRunLoop::collect() {
    if (callbacks_running_ == 0) {
        retired_timers_.clear();
        retired_watches_.clear();
    }
}

std::uint64_t HostRunLoop::add_timer(std::chrono::milliseconds period, std::function<void()> tick) {
    if (loop_ == nullptr || !tick)
        return 0;
    auto timer = std::make_unique<Timer>(*this, std::move(tick));
    const auto ms = static_cast<Linux::TimerInterval>(std::max<long long>(1, period.count()));
    if (loop_->registerTimer(timer.get(), ms) != kResultTrue)
        return 0;
    const auto id = next_id_++;
    timers_[id] = std::move(timer);
    return id;
}

void HostRunLoop::remove_timer(std::uint64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end())
        return;
    loop_->unregisterTimer(it->second.get());
    retired_timers_.push_back(std::move(it->second));
    timers_.erase(it);
    collect();
}

std::uint64_t HostRunLoop::add_fd(int fd, std::function<void()> readable) {
    if (loop_ == nullptr || !readable)
        return 0;
    auto watch = std::make_unique<Watch>(*this, std::move(readable));
    if (loop_->registerEventHandler(watch.get(), fd) != kResultTrue)
        return 0;
    const auto id = next_id_++;
    watches_[id] = std::move(watch);
    return id;
}

void HostRunLoop::remove_fd(std::uint64_t id) {
    auto it = watches_.find(id);
    if (it == watches_.end())
        return;
    loop_->unregisterEventHandler(it->second.get());
    retired_watches_.push_back(std::move(it->second));
    watches_.erase(it);
    collect();
}

} // namespace pulp::format::vst3
