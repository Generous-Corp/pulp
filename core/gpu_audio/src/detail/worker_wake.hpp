#pragma once

#include <chrono>
#include <cstdint>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore>
#endif

namespace pulp::gpu_audio::detail {

// Counting wake for the transport worker. post() never allocates or blocks
// and may be called from any thread, including the audio callback and a GPU
// backend's callback thread; while a waiter is parked it is a kernel call (a
// Mach semaphore signal on Apple, the platform's semaphore wake elsewhere).
// wait_for() blocks in the kernel until a post or the timeout. On Apple it is
// a dispatch semaphore: libc++'s timed
// counting_semaphore wait polls with a sleeping backoff there, which adds
// hundreds of microseconds to a wake that has already been posted.
class WorkerWake {
  public:
    WorkerWake() noexcept
#if defined(__APPLE__)
        : semaphore_(dispatch_semaphore_create(0))
#endif
    {
    }
    ~WorkerWake() {
#if defined(__APPLE__)
        if (semaphore_ != nullptr)
            dispatch_release(semaphore_);
#endif
    }
    WorkerWake(const WorkerWake&) = delete;
    WorkerWake& operator=(const WorkerWake&) = delete;

    void post() noexcept {
#if defined(__APPLE__)
        (void)dispatch_semaphore_signal(semaphore_);
#else
        semaphore_.release();
#endif
    }

    // Nonblocking: consume one pending post.
    bool try_consume() noexcept {
#if defined(__APPLE__)
        return dispatch_semaphore_wait(semaphore_, DISPATCH_TIME_NOW) == 0;
#else
        return semaphore_.try_acquire();
#endif
    }

    // Consume one post, waiting up to `timeout` for it.
    bool wait_for(std::chrono::microseconds timeout) noexcept {
#if defined(__APPLE__)
        const auto ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
        return dispatch_semaphore_wait(semaphore_, dispatch_time(DISPATCH_TIME_NOW, ns)) == 0;
#else
        return semaphore_.try_acquire_for(timeout);
#endif
    }

  private:
#if defined(__APPLE__)
    dispatch_semaphore_t semaphore_ = nullptr;
#else
    std::counting_semaphore<> semaphore_{0};
#endif
};

} // namespace pulp::gpu_audio::detail
