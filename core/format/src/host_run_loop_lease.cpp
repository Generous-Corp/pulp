#include "host_run_loop_lease.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace pulp::format::detail {

namespace {

// The live leases' scopes, innermost last. Handles are weak, so a post that
// races a lease's destruction is refused rather than run.
struct Posters {
    std::mutex mutex;
    std::vector<std::pair<platform::HostRunLoopToken, platform::MainThreadScope::Handle>> list;
};

Posters& posters() {
    // Never destroyed: a lease in a plug-in's statics may outlive main().
    static Posters* p = new Posters;
    return *p;
}

} // namespace

HostRunLoopLease::HostRunLoopLease(platform::HostRunLoop& loop, bool process_scope)
    : token_(platform::register_host_run_loop(loop)) {
    if (token_ == 0 || !process_scope)
        return;
    scope_ = std::make_unique<platform::MainThreadScope>();
    Posters& p = posters();
    std::lock_guard<std::mutex> lock(p.mutex);
    p.list.emplace_back(token_, scope_->handle());
}

HostRunLoopLease::~HostRunLoopLease() {
    if (token_ == 0)
        return;
    if (scope_) {
        Posters& p = posters();
        std::lock_guard<std::mutex> lock(p.mutex);
        p.list.erase(std::remove_if(p.list.begin(), p.list.end(),
                                    [&](const auto& e) { return e.first == token_; }),
                     p.list.end());
    }
    scope_.reset();
    platform::unregister_host_run_loop(token_);
}

bool post_through_host_run_loop(std::function<void()>& fn) {
    platform::MainThreadScope::Handle handle;
    {
        Posters& p = posters();
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.list.empty())
            return false;
        handle = p.list.back().second;
    }
    // The task and this frame share one box; the task empties it after
    // running, so whichever side lets go last frees only an empty function.
    auto box = std::make_shared<std::function<void()>>(std::move(fn));
    fn = nullptr;
    if (handle.post([box] {
            auto task = std::move(*box);
            *box = nullptr;
            task();
        }))
        return true;
    fn = std::move(*box);
    return false;
}

} // namespace pulp::format::detail
