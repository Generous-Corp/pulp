// Runtime provider faults for the GPU fault-continuity test plug-in.
//
// DawnSharedIoProvider's own fault hooks are construction-time Options that
// fire on the first submission, so they cannot fault a running provider at a
// chosen sample, and the provider source is not edited here. Instead this
// test-only plug-in interposes Dawn's process-wide proc table, after the
// provider has installed it, with wrappers that forward every call and, when
// armed, apply one fault at the same call sites the provider's own Fault
// enum uses:
//
//   force_loss_before   deviceForceLoss before the next queueSubmit
//   force_loss_between  deviceForceLoss before the next queueOnSubmittedWorkDone
//   force_loss_after    deviceForceLoss after the next queueOnSubmittedWorkDone
//   delay_completion    the next instanceProcessEvents sleeps `delay_ms` first,
//                       so the provider observes completions that much late
//   hang                the same, for long enough that the worker is stuck
//                       well past the transport's lead
//
// The table is re-asserted every time a fault is armed: the editor's render
// surface reinstalls Dawn's native table each time it initializes
// (gpu_surface_dawn.cpp), which silently removed the interposer in REAPER,
// where the editor opens after the plug-in prepares.
//
// Only calls made off the main thread are faulted. The shared-I/O provider is
// driven from the transport's worker thread, while the editor's Skia/Dawn
// render device is driven from the main thread, so a render-side call is never
// touched by an audio fault.

#include "gpu_fault_interposer.hpp"

#if defined(PULP_GPU_FAULT_HAS_DAWN)
#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"

#include <pthread.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace pulp::examples::gpu_fault {
namespace {

std::atomic<std::uint8_t> g_armed{0}; // FaultKind, 0 = none
std::atomic<std::uint32_t> g_delay_ms{0};
std::atomic<std::uint64_t> g_applied{0};
std::atomic<const char*> g_applied_point{""};
std::once_flag g_install_once;
bool g_installed = false;

#if defined(PULP_GPU_FAULT_HAS_DAWN)
DawnProcTable g_native{};
DawnProcTable g_wrapped{};
// Queue -> device, recorded from deviceGetQueue on any thread, because a host
// may prepare (and so create the device and fetch its queue) on the main
// thread while the worker submits. Small and fixed; never touched by audio.
std::mutex g_queues_mutex;
std::array<std::pair<WGPUQueue, WGPUDevice>, 64> g_queues{};
std::size_t g_queues_next = 0;

// The device whose command encoder this thread created last. Covers a queue
// fetched before the interposer was installed (a single prepare), when the
// provider records its encoders on the submitting worker thread.
thread_local WGPUDevice t_device = nullptr;

WGPUDevice device_for(WGPUQueue queue) {
    std::lock_guard lock(g_queues_mutex);
    for (const auto& [q, d] : g_queues)
        if (q == queue)
            return d;
    return t_device;
}

WGPUCommandEncoder create_command_encoder(WGPUDevice device,
                                          WGPUCommandEncoderDescriptor const* descriptor) {
    if (pthread_main_np() == 0)
        t_device = device;
    return g_native.deviceCreateCommandEncoder(device, descriptor);
}

bool off_main() noexcept {
    return pthread_main_np() == 0;
}

// Claim the armed fault if it is `kind`; exactly one call applies it. A loss
// is claimed only when the queue's device is known, so it is never consumed
// without being applied.
bool take(FaultKind kind, bool ready = true) noexcept {
    auto expected = static_cast<std::uint8_t>(kind);
    return ready && off_main() &&
           g_armed.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

void applied(const char* point) noexcept {
    g_applied_point.store(point, std::memory_order_release);
    g_applied.fetch_add(1, std::memory_order_acq_rel);
}

void force_loss(WGPUDevice device, const char* point) noexcept {
    g_native.deviceForceLoss(device, WGPUDeviceLostReason_Unknown,
                             WGPUStringView{"WP-h1 provider fault", WGPU_STRLEN});
    applied(point);
}

WGPUQueue get_queue(WGPUDevice device) {
    WGPUQueue queue = g_native.deviceGetQueue(device);
    std::lock_guard lock(g_queues_mutex);
    for (const auto& [q, d] : g_queues)
        if (q == queue)
            return queue;
    g_queues[g_queues_next++ % g_queues.size()] = {queue, device};
    return queue;
}

void queue_submit(WGPUQueue queue, size_t count, WGPUCommandBuffer const* commands) {
    if (WGPUDevice device = device_for(queue); take(FaultKind::ForceLossBefore, device != nullptr))
        force_loss(device, "before_submit");
    g_native.queueSubmit(queue, count, commands);
}

WGPUFuture on_submitted_work_done(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo info) {
    WGPUDevice device = device_for(queue);
    if (take(FaultKind::ForceLossBetween, device != nullptr))
        force_loss(device, "between_submit_and_completion_registration");
    const bool after = take(FaultKind::ForceLossAfter, device != nullptr);
    const WGPUFuture future = g_native.queueOnSubmittedWorkDone(queue, info);
    if (after)
        force_loss(device, "after_completion_registration");
    return future;
}

void process_events(WGPUInstance instance) {
    if (take(FaultKind::DelayCompletion) || take(FaultKind::Hang)) {
        applied("process_events_stall");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(g_delay_ms.load(std::memory_order_acquire)));
    }
    g_native.instanceProcessEvents(instance);
}
#endif

} // namespace

bool install_provider_fault_interposer(std::string& detail) {
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    std::call_once(g_install_once, [] {
        // Called after the shared-I/O provider prepared, so its one-time
        // process installation of the exact native table has already happened
        // and will not overwrite this one.
        g_native = dawn::native::GetProcs();
        g_wrapped = g_native;
        g_wrapped.deviceGetQueue = &get_queue;
        g_wrapped.deviceCreateCommandEncoder = &create_command_encoder;
        g_wrapped.queueSubmit = &queue_submit;
        g_wrapped.queueOnSubmittedWorkDone = &on_submitted_work_done;
        g_wrapped.instanceProcessEvents = &process_events;
        dawnProcSetProcs(&g_wrapped);
        g_installed = true;
    });
    detail = g_installed ? "dawn proc-table interposer installed" : "interposer install failed";
    return g_installed;
#else
    detail = "built without Dawn headers";
    return false;
#endif
}

bool provider_fault_interposer_installed() noexcept {
    return g_installed;
}

bool arm_provider_fault(FaultKind kind, std::uint32_t delay_ms, std::string& detail) {
    if (!g_installed) {
        detail = "the provider fault interposer is not installed";
        return false;
    }
    if (kind == FaultKind::None || kind == FaultKind::RenderForceLoss) {
        detail = "not a provider fault";
        return false;
    }
    g_delay_ms.store(delay_ms, std::memory_order_release);
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    dawnProcSetProcs(&g_wrapped);
#endif
    std::uint8_t expected = 0;
    if (!g_armed.compare_exchange_strong(expected, static_cast<std::uint8_t>(kind),
                                         std::memory_order_acq_rel)) {
        detail = "a provider fault is already armed";
        return false;
    }
    detail = "proc table re-asserted; armed at the provider's next matching call on the "
             "worker thread";
    return true;
}

void disarm_provider_fault() noexcept {
    g_armed.store(0, std::memory_order_release);
}

std::uint64_t provider_faults_applied() noexcept {
    return g_applied.load(std::memory_order_acquire);
}

const char* provider_fault_point() noexcept {
    return g_applied_point.load(std::memory_order_acquire);
}

} // namespace pulp::examples::gpu_fault
