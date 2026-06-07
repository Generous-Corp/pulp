#include "udev_monitor.hpp"

#include <pulp/runtime/log.hpp>

#include <cstring>

#include <poll.h>
#include <unistd.h>

namespace pulp::audio::linux_platform {

UdevChange classify_udev_action(const char* action) noexcept {
    if (action == nullptr) return UdevChange::other;
    if (std::strcmp(action, "add") == 0) return UdevChange::added;
    if (std::strcmp(action, "remove") == 0) return UdevChange::removed;
    return UdevChange::other;
}

// ── libudev entry points (resolved at runtime; opaque types as void*) ───────
struct UdevMonitor::Api {
    using fn_udev_new        = void* (*)();
    using fn_udev_unref      = void* (*)(void*);
    using fn_mon_new         = void* (*)(void*, const char*);
    using fn_mon_filter      = int   (*)(void*, const char*, const char*);
    using fn_mon_enable      = int   (*)(void*);
    using fn_mon_get_fd      = int   (*)(void*);
    using fn_mon_receive     = void* (*)(void*);
    using fn_mon_unref       = void* (*)(void*);
    using fn_dev_get_action  = const char* (*)(void*);
    using fn_dev_unref       = void* (*)(void*);

    fn_udev_new       udev_new = nullptr;
    fn_udev_unref     udev_unref = nullptr;
    fn_mon_new        monitor_new = nullptr;
    fn_mon_filter     monitor_filter = nullptr;
    fn_mon_enable     monitor_enable = nullptr;
    fn_mon_get_fd     monitor_get_fd = nullptr;
    fn_mon_receive    monitor_receive = nullptr;
    fn_mon_unref      monitor_unref = nullptr;
    fn_dev_get_action device_get_action = nullptr;
    fn_dev_unref      device_unref = nullptr;

    bool complete() const {
        return udev_new && udev_unref && monitor_new && monitor_filter &&
               monitor_enable && monitor_get_fd && monitor_receive &&
               monitor_unref && device_get_action && device_unref;
    }
};

namespace {
// libudev SONAME is libudev.so.1 across all modern distros; the unversioned
// libudev.so only ships with -dev. Try both so a runtime-only host still works.
bool open_libudev(runtime::DynamicLibrary& lib) {
    return lib.open("libudev.so.1") || lib.open("libudev.so");
}

template <typename Fn>
Fn sym(runtime::DynamicLibrary& lib, const char* name) {
    return reinterpret_cast<Fn>(lib.find_symbol(name));
}
}  // namespace

bool UdevMonitor::library_available() {
    runtime::DynamicLibrary lib;
    return open_libudev(lib);
}

UdevMonitor::~UdevMonitor() { stop(); }

bool UdevMonitor::start(const std::vector<std::string>& subsystems,
                        ChangeCallback on_change) {
    if (running_.load(std::memory_order_acquire)) return false;
    if (!open_libudev(lib_)) {
        runtime::log_info("udev: libudev unavailable — device hotplug disabled");
        return false;
    }

    auto api = new Api();
    api->udev_new          = sym<Api::fn_udev_new>(lib_, "udev_new");
    api->udev_unref        = sym<Api::fn_udev_unref>(lib_, "udev_unref");
    api->monitor_new       = sym<Api::fn_mon_new>(lib_, "udev_monitor_new_from_netlink");
    api->monitor_filter    = sym<Api::fn_mon_filter>(lib_, "udev_monitor_filter_add_match_subsystem_devtype");
    api->monitor_enable    = sym<Api::fn_mon_enable>(lib_, "udev_monitor_enable_receiving");
    api->monitor_get_fd    = sym<Api::fn_mon_get_fd>(lib_, "udev_monitor_get_fd");
    api->monitor_receive   = sym<Api::fn_mon_receive>(lib_, "udev_monitor_receive_device");
    api->monitor_unref     = sym<Api::fn_mon_unref>(lib_, "udev_monitor_unref");
    api->device_get_action = sym<Api::fn_dev_get_action>(lib_, "udev_device_get_action");
    api->device_unref      = sym<Api::fn_dev_unref>(lib_, "udev_device_unref");
    if (!api->complete()) {
        runtime::log_warn("udev: libudev symbols incomplete — hotplug disabled");
        delete api; lib_.close(); return false;
    }

    udev_ = api->udev_new();
    if (!udev_) { delete api; lib_.close(); return false; }
    monitor_ = api->monitor_new(udev_, "udev");
    if (!monitor_) {
        api->udev_unref(udev_); udev_ = nullptr; delete api; lib_.close(); return false;
    }
    for (const auto& s : subsystems)
        api->monitor_filter(monitor_, s.c_str(), nullptr);
    if (api->monitor_enable(monitor_) < 0) {
        api->monitor_unref(monitor_); monitor_ = nullptr;
        api->udev_unref(udev_); udev_ = nullptr; delete api; lib_.close(); return false;
    }
    fd_ = api->monitor_get_fd(monitor_);
    if (fd_ < 0 || pipe(wake_pipe_) != 0) {
        api->monitor_unref(monitor_); monitor_ = nullptr;
        api->udev_unref(udev_); udev_ = nullptr; delete api; lib_.close(); return false;
    }

    api_ = api;
    on_change_ = std::move(on_change);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run_loop(); });
    return true;
}

void UdevMonitor::run_loop() {
    struct pollfd fds[2];
    fds[0].fd = fd_;            fds[0].events = POLLIN;
    fds[1].fd = wake_pipe_[0];  fds[1].events = POLLIN;
    while (running_.load(std::memory_order_relaxed)) {
        int n = ::poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) break;  // stop() signalled
        if (fds[0].revents & POLLIN) {
            void* dev = api_->monitor_receive(monitor_);
            if (!dev) continue;
            const UdevChange change = classify_udev_action(api_->device_get_action(dev));
            api_->device_unref(dev);
            if ((change == UdevChange::added || change == UdevChange::removed) && on_change_)
                on_change_(change);
        }
    }
}

void UdevMonitor::stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        if (wake_pipe_[1] >= 0) { char c = 1; ssize_t w = ::write(wake_pipe_[1], &c, 1); (void)w; }
        if (thread_.joinable()) thread_.join();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    if (wake_pipe_[0] >= 0) { ::close(wake_pipe_[0]); wake_pipe_[0] = -1; }
    if (wake_pipe_[1] >= 0) { ::close(wake_pipe_[1]); wake_pipe_[1] = -1; }
    if (api_) {
        if (monitor_) { api_->monitor_unref(monitor_); monitor_ = nullptr; }
        if (udev_)    { api_->udev_unref(udev_); udev_ = nullptr; }
        delete api_; api_ = nullptr;
    }
    fd_ = -1;
    on_change_ = nullptr;
    lib_.close();
}

}  // namespace pulp::audio::linux_platform
