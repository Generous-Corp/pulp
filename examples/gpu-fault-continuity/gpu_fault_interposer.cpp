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
//   worker_stall        the same, for seconds: the worker is stuck well past
//                       the transport's lead while the GPU itself is fine
//   missing_completion  the next queueOnSubmittedWorkDone callback is
//                       swallowed, so that submission never reports done (the
//                       hung-device route)
//
// The table is re-asserted every time a fault is armed: the editor's render
// surface reinstalls Dawn's native table each time it initializes
// (gpu_surface_dawn.cpp), which silently removed the interposer in REAPER,
// where the editor opens after the plug-in prepares.
//
// Accepted test-only races and limits:
//  - Re-asserting the table is a plain struct copy into Dawn's global table
//    while other threads may be calling wgpu*. Each entry is a pointer-sized
//    store, and a mixed native/wrapped table is harmless because every wrapper
//    forwards to the native function, so a call sees either one. ThreadSanitizer
//    will still report it. If the editor reinstalls the native table between
//    arm and apply, the fault is never applied and the run is INCONCLUSIVE.
//  - Faults are targeted: the plug-in captures its own provider's device (the
//    one whose queue is fetched while its node prepares) and the instance
//    behind it, and a fault is taken only by a call on that device or
//    instance. Another plug-in instance or Dawn client in the process is never
//    faulted.
//
// Only calls made off the main thread are faulted. The shared-I/O provider is
// driven from the transport's worker thread, while the editor's Skia/Dawn
// render device is driven from the main thread, so a render-side call is never
// touched by an audio fault.

#include "gpu_fault_interposer.hpp"

// This file must never reach a shipped product: it rewrites Dawn's process-wide
// proc table to inject device loss. Only the test-only plug-in's build defines
// the macro below.
#if !defined(PULP_GPU_FAULT_TEST_ONLY)
#error "gpu_fault_interposer.cpp is test-only; build it only with PULP_GPU_FAULT_TEST_ONLY"
#endif

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
std::atomic<std::uintptr_t> g_applied_device{0};
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

// This plug-in instance's provider, captured while its node prepares.
// Only the thread that is preparing this instance's node captures, so a
// device another thread creates meanwhile is never mistaken for ours.
thread_local bool t_capturing = false;
std::atomic<WGPUDevice> g_target_device{nullptr};
std::atomic<WGPUInstance> g_target_instance{nullptr};

// The device whose command encoder this thread created last. Covers a queue
// fetched before the interposer was installed (a single prepare), when the
// provider records its encoders on the submitting worker thread.
thread_local WGPUDevice t_device = nullptr;

// The submitting worker's own device wins; the table covers a worker that
// fetched no encoder yet. Both are reset when the plug-in releases its engine,
// so a recycled queue address can never map to a freed device.
WGPUDevice device_for(WGPUQueue queue) {
    if (t_device != nullptr)
        return t_device;
    std::lock_guard lock(g_queues_mutex);
    for (const auto& [q, d] : g_queues)
        if (q == queue)
            return d;
    return nullptr;
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

bool is_target(WGPUDevice device) noexcept {
    return device != nullptr && device == g_target_device.load(std::memory_order_acquire);
}

void applied(const char* point) noexcept {
    g_applied_point.store(point, std::memory_order_release);
    g_applied.fetch_add(1, std::memory_order_acq_rel);
}

void force_loss(WGPUDevice device, const char* point) noexcept {
    g_native.deviceForceLoss(device, WGPUDeviceLostReason_Unknown,
                             WGPUStringView{"WP-h1 provider fault", WGPU_STRLEN});
    g_applied_device.store(reinterpret_cast<std::uintptr_t>(device), std::memory_order_release);
    applied(point);
}

void swallow_work_done(WGPUQueueWorkDoneStatus, WGPUStringView, void*, void*) {}

WGPUQueue get_queue(WGPUDevice device) {
    WGPUQueue queue = g_native.deviceGetQueue(device);
    if (t_capturing && g_target_device.load(std::memory_order_acquire) == nullptr)
        g_target_device.store(device, std::memory_order_release);
    std::lock_guard lock(g_queues_mutex);
    for (auto& [q, d] : g_queues) {
        if (q == queue) {
            d = device; // a recycled queue address belongs to the new device
            return queue;
        }
    }
    g_queues[g_queues_next++ % g_queues.size()] = {queue, device};
    return queue;
}

void queue_submit(WGPUQueue queue, size_t count, WGPUCommandBuffer const* commands) {
    if (WGPUDevice device = device_for(queue); take(FaultKind::ForceLossBefore, is_target(device)))
        force_loss(device, "before_submit");
    g_native.queueSubmit(queue, count, commands);
}

WGPUFuture on_submitted_work_done(WGPUQueue queue, WGPUQueueWorkDoneCallbackInfo info) {
    WGPUDevice device = device_for(queue);
    if (take(FaultKind::ForceLossBetween, is_target(device)))
        force_loss(device, "between_submit_and_completion_registration");
    const bool after = take(FaultKind::ForceLossAfter, is_target(device));
    if (take(FaultKind::MissingCompletion, is_target(device))) {
        // The work still completes on the GPU; the provider just never hears.
        info.callback = &swallow_work_done;
        g_applied_device.store(reinterpret_cast<std::uintptr_t>(device), std::memory_order_release);
        applied("completion_callback_swallowed");
    }
    const WGPUFuture future = g_native.queueOnSubmittedWorkDone(queue, info);
    if (after)
        force_loss(device, "after_completion_registration");
    return future;
}

void process_events(WGPUInstance instance) {
    const bool mine =
        instance != nullptr && instance == g_target_instance.load(std::memory_order_acquire);
    if (take(FaultKind::DelayCompletion, mine) || take(FaultKind::WorkerStall, mine)) {
        g_applied_device.store(
            reinterpret_cast<std::uintptr_t>(g_target_device.load(std::memory_order_acquire)),
            std::memory_order_release);
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

void begin_provider_fault_target_capture() noexcept {
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    if (!g_installed)
        return;
    // The editor may have reinstalled the native table since the last arm.
    dawnProcSetProcs(&g_wrapped);
    g_target_device.store(nullptr, std::memory_order_release);
    g_target_instance.store(nullptr, std::memory_order_release);
    t_capturing = true;
#endif
}

bool end_provider_fault_target_capture(std::string& detail) {
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    t_capturing = false;
    WGPUDevice device = g_target_device.load(std::memory_order_acquire);
    if (device == nullptr) {
        detail = "the provider's device was not seen while the node prepared";
        return false;
    }
    // The instance the provider services completions on, for the stall faults.
    WGPUAdapter adapter = g_native.deviceGetAdapter(device);
    WGPUInstance instance = adapter ? g_native.adapterGetInstance(adapter) : nullptr;
    g_target_instance.store(instance, std::memory_order_release);
    if (instance != nullptr)
        g_native.instanceRelease(instance);
    if (adapter != nullptr)
        g_native.adapterRelease(adapter);
    detail = instance != nullptr ? "dawn proc-table interposer installed; provider targeted"
                                 : "provider device captured but not its instance";
    return instance != nullptr;
#else
    detail = "built without Dawn headers";
    return false;
#endif
}

std::uint64_t provider_fault_target_device() noexcept {
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    return reinterpret_cast<std::uintptr_t>(g_target_device.load(std::memory_order_acquire));
#else
    return 0;
#endif
}

std::uint64_t provider_fault_applied_device() noexcept {
    return g_applied_device.load(std::memory_order_acquire);
}

bool provider_fault_target_captured() noexcept {
    return g_target_instance.load(std::memory_order_acquire) != nullptr;
}

bool provider_fault_interposer_installed() noexcept {
    return g_installed;
}

bool arm_provider_fault(FaultKind kind, std::uint32_t delay_ms, std::string& detail) {
    if (!g_installed) {
        detail = "the provider fault interposer is not installed";
        return false;
    }
    // Only the six provider faults; anything else would sit armed forever.
    switch (kind) {
    case FaultKind::ForceLossBefore:
    case FaultKind::ForceLossBetween:
    case FaultKind::ForceLossAfter:
    case FaultKind::DelayCompletion:
    case FaultKind::WorkerStall:
    case FaultKind::MissingCompletion:
        break;
    default:
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
#if defined(PULP_GPU_FAULT_HAS_DAWN)
    // The engine is being released: its device and queue are about to be
    // freed, so no mapping to them may survive.
    std::lock_guard lock(g_queues_mutex);
    g_queues = {};
    g_queues_next = 0;
    g_target_device.store(nullptr, std::memory_order_release);
    g_target_instance.store(nullptr, std::memory_order_release);
#endif
}

std::uint64_t provider_faults_applied() noexcept {
    return g_applied.load(std::memory_order_acquire);
}

const char* provider_fault_point() noexcept {
    return g_applied_point.load(std::memory_order_acquire);
}

} // namespace pulp::examples::gpu_fault
