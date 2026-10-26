#pragma once

namespace pulp::gpu_audio::detail {

// "A completion may now be serviceable." A provider that has a completion
// event source calls this when its backend reports finished GPU work, and the
// call happens-before the dispatcher's
// next service step. It does not mean a terminal record was pushed: the
// dispatcher re-checks the inbox, spurious calls are allowed, and a wake is
// never progress. Terminals and the unresponsive-provider watchdog come only
// from service steps. Callable from any thread, including the backend's
// completion thread. It must not allocate, lock, block or call into the
// backend; posting a semaphore is fine. No shipped provider installs one yet
// (the Dawn and CPU shared-I/O providers keep the default
// set_completion_notifier, which returns false), so
// a transport serving them polls for completions.
using SharedIoCompletionNotifyFn = void (*)(void* context) noexcept;

struct SharedIoCompletionNotifier {
    SharedIoCompletionNotifyFn notify = nullptr;
    void* context = nullptr;
};

} // namespace pulp::gpu_audio::detail
