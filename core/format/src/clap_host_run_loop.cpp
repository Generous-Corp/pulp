#include "clap_host_run_loop.hpp"

#include <algorithm>
#include <utility>

namespace pulp::format::clap_adapter {

ClapHostRunLoop::ClapHostRunLoop(const clap_host_t* host) : host_(host) {
    if (host_ == nullptr || host_->get_extension == nullptr)
        return;
    const auto* timer = static_cast<const clap_host_timer_support_t*>(
        host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
    if (timer != nullptr && timer->register_timer != nullptr && timer->unregister_timer != nullptr)
        timer_ = timer;
    const auto* fd = static_cast<const clap_host_posix_fd_support_t*>(
        host_->get_extension(host_, CLAP_EXT_POSIX_FD_SUPPORT));
    if (fd != nullptr && fd->register_fd != nullptr && fd->unregister_fd != nullptr)
        fd_ = fd;
}

ClapHostRunLoop::~ClapHostRunLoop() {
    // The platform layer removes what it added first; withdraw anything left.
    for (auto& entry : timers_)
        timer_->unregister_timer(host_, entry.first);
    for (auto& entry : watches_)
        fd_->unregister_fd(host_, entry.first);
}

bool ClapHostRunLoop::on_ui_thread() const {
    return std::this_thread::get_id() == ui_;
}

std::uint64_t ClapHostRunLoop::add_timer(std::chrono::milliseconds period,
                                         std::function<void()> tick) {
    if (timer_ == nullptr || !tick)
        return 0;
    clap_id id = CLAP_INVALID_ID;
    const auto ms = static_cast<std::uint32_t>(std::max<long long>(1, period.count()));
    if (!timer_->register_timer(host_, ms, &id) || id == CLAP_INVALID_ID)
        return 0;
    timers_[id] = std::move(tick);
    return static_cast<std::uint64_t>(id) + 1; // 0 is "refused"
}

void ClapHostRunLoop::remove_timer(std::uint64_t id) {
    if (id == 0 || timer_ == nullptr)
        return;
    const auto host_id = static_cast<clap_id>(id - 1);
    if (timers_.erase(host_id) != 0)
        timer_->unregister_timer(host_, host_id);
}

void ClapHostRunLoop::on_timer(clap_id id) {
    auto it = timers_.find(id);
    if (it == timers_.end())
        return;
    // Copy first: the callback may remove its own timer.
    auto tick = it->second;
    tick();
}

std::uint64_t ClapHostRunLoop::add_fd(int fd, std::function<void()> readable) {
    if (fd_ == nullptr || fd < 0 || !readable || watches_.count(fd) != 0)
        return 0;
    if (!fd_->register_fd(host_, fd, CLAP_POSIX_FD_READ))
        return 0;
    watches_[fd] = std::move(readable);
    return static_cast<std::uint64_t>(fd) + 1;
}

void ClapHostRunLoop::remove_fd(std::uint64_t id) {
    if (id == 0 || fd_ == nullptr)
        return;
    const int fd = static_cast<int>(id - 1);
    if (watches_.erase(fd) != 0)
        fd_->unregister_fd(host_, fd);
}

void ClapHostRunLoop::on_fd(int fd, clap_posix_fd_flags_t flags) {
    if ((flags & (CLAP_POSIX_FD_READ | CLAP_POSIX_FD_ERROR)) == 0)
        return;
    auto it = watches_.find(fd);
    if (it == watches_.end())
        return;
    auto readable = it->second;
    readable();
}

} // namespace pulp::format::clap_adapter
