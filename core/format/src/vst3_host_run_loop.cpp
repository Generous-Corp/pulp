#include "vst3_host_run_loop.hpp"

#include <algorithm>
#include <utility>

namespace pulp::format::vst3 {

using namespace Steinberg;

// The host holds these by raw pointer between registerTimer and
// unregisterTimer, so reference counting is not theirs to drive.
class HostRunLoop::Timer final : public Linux::ITimerHandler {
  public:
    Timer(std::shared_ptr<Shared> shared, std::function<void()> tick)
        : shared_(std::move(shared)), tick_(std::move(tick)) {}

    void PLUGIN_API onTimer() override {
        // The tick may remove this timer or destroy the whole loop, and with
        // them `tick_` and `this`: hold what is needed on the stack, and touch
        // nothing of this object after collect().
        auto shared = shared_;
        auto tick = tick_;
        ++shared->callbacks_running;
        if (tick)
            tick();
        --shared->callbacks_running;
        shared->collect();
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
    std::shared_ptr<Shared> shared_;
    std::function<void()> tick_;
};

class HostRunLoop::Watch final : public Linux::IEventHandler {
  public:
    Watch(std::shared_ptr<Shared> shared, std::function<void()> readable)
        : shared_(std::move(shared)), readable_(std::move(readable)) {}

    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) override {
        auto shared = shared_;
        auto readable = readable_;
        ++shared->callbacks_running;
        if (readable)
            readable();
        --shared->callbacks_running;
        shared->collect();
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
    std::shared_ptr<Shared> shared_;
    std::function<void()> readable_;
};

HostRunLoop::HostRunLoop(Linux::IRunLoop* run_loop) : loop_(run_loop) {
    if (loop_ != nullptr)
        loop_->addRef();
}

HostRunLoop::~HostRunLoop() {
    // The platform layer removes everything it added before it lets go of us;
    // anything left is withdrawn here so the host keeps no pointer into us.
    // This may run from inside a host callback (a tick that destroyed the view):
    // the handlers move to the shared state, which frees them once no callback
    // is running.
    for (auto& entry : timers_) {
        loop_->unregisterTimer(entry.second.get());
        shared_->retired_timers.push_back(std::move(entry.second));
    }
    for (auto& entry : watches_) {
        loop_->unregisterEventHandler(entry.second.get());
        shared_->retired_watches.push_back(std::move(entry.second));
    }
    timers_.clear();
    watches_.clear();
    if (loop_ != nullptr)
        loop_->release();
    shared_->collect();
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

HostRunLoop::Shared::Shared() = default;
HostRunLoop::Shared::~Shared() = default;

void HostRunLoop::Shared::collect() {
    if (callbacks_running == 0) {
        // Move out first: freeing a handler must not re-enter this list.
        auto timers = std::move(retired_timers);
        auto watches = std::move(retired_watches);
        retired_timers.clear();
        retired_watches.clear();
    }
}

std::uint64_t HostRunLoop::add_timer(std::chrono::milliseconds period, std::function<void()> tick) {
    if (loop_ == nullptr || !tick)
        return 0;
    auto timer = std::make_unique<Timer>(shared_, std::move(tick));
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
    shared_->retired_timers.push_back(std::move(it->second));
    timers_.erase(it);
    shared_->collect();
}

std::uint64_t HostRunLoop::add_fd(int fd, std::function<void()> readable) {
    if (loop_ == nullptr || !readable)
        return 0;
    auto watch = std::make_unique<Watch>(shared_, std::move(readable));
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
    shared_->retired_watches.push_back(std::move(it->second));
    watches_.erase(it);
    shared_->collect();
}

} // namespace pulp::format::vst3
