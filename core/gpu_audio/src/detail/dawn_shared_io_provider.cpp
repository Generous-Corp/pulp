#include "dawn_shared_io_provider.hpp"
#include "dawn_shared_io_convolution_session.hpp"
#include "dawn_shared_io_wavenet_program.hpp"

#include "dawn_submission_tracker.hpp"

#include "dawn/dawn_proc.h"
#include "dawn/dawn_version.h"
#include "dawn/native/DawnNative.h"
#include "webgpu/webgpu_cpp.h"

#if defined(PULP_GPU_AUDIO_HAS_VELLUM_D15)
#include <vellum/graphics/dawn_bootstrap.hpp>
#include <vellum/graphics/dawn_native_bootstrap.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::gpu_audio::detail {

namespace {

constexpr auto kDrainLimit = std::chrono::seconds(15);
constexpr auto kMaxCompletionWaitNs =
    static_cast<std::uint64_t>(std::chrono::nanoseconds::max().count());
constexpr std::size_t kWaitAnyBatchSize = 64;
constexpr std::uint64_t kDefaultCompletionWaitNs = 1'000'000;

std::string revision(const std::uint8_t* bytes) {
    if (bytes == nullptr)
        return {};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < dawn::kDawnVersion.size(); ++i)
        out << std::setw(2) << unsigned(bytes[i]);
    return out.str();
}

struct ProcBootstrap {
    std::mutex mutex;
    std::string revision;
    std::uint64_t installs = 0;
};

ProcBootstrap& proc_bootstrap() {
    static ProcBootstrap state;
    return state;
}

#if defined(PULP_GPU_AUDIO_HAS_VELLUM_D15)
// The installed Vellum coordinator owns the process state machine.  Pulp only
// supplies this callback for the test-only proc-table interposer; production
// creation uses vellum::app_host::register_native_dawn_bootstrap below.  Both
// routes therefore pass through Vellum's revision/re-entry/exception boundary.
struct VellumOverrideBootstrap {
    std::string expected;
    const DawnProcTable* override = nullptr;
};

vellum::graphics::DawnBootstrapResult
install_vellum_override(const vellum::graphics::DawnBootstrapRequest& request, void* opaque,
                        std::string* error) {
    const auto* context = static_cast<const VellumOverrideBootstrap*>(opaque);
    const auto header = revision(dawn::kDawnVersion.data());
    const auto proc = revision(dawnProcGetVersion());
    const DawnProcTable& native = dawn::native::GetProcs();
    const auto native_revision = revision(native.version);
    const auto* selected =
        context == nullptr || context->override == nullptr ? &native : context->override;
    if (context == nullptr || request.abi_version != vellum::graphics::kDawnBootstrapAbiVersion ||
        request.expected_dawn_revision != context->expected || header.empty() || header != proc ||
        header != native_revision || header != context->expected ||
        revision(selected->version) != header) {
        if (error != nullptr)
            *error = "provider_identity_mismatch";
        return vellum::graphics::DawnBootstrapResult::identity_mismatch;
    }
    dawnProcSetProcs(selected);
    return vellum::graphics::DawnBootstrapResult::ready;
}
#endif

bool install_exact_proc_table(const std::string& expected, const void* override_for_testing,
                              std::string& reason) {
#if defined(PULP_GPU_AUDIO_HAS_VELLUM_D15)
    const auto header = vellum::app_host::native_dawn_revision();
    if (header.empty() || (!expected.empty() && expected != header)) {
        reason = "provider_identity_mismatch";
        return false;
    }

    std::string coordinator_error;
    bool registered = false;
    if (override_for_testing != nullptr) {
        VellumOverrideBootstrap context{
            .expected = header,
            .override = static_cast<const DawnProcTable*>(override_for_testing),
        };
        registered = vellum::graphics::register_dawn_bootstrap(
            {.abi_version = vellum::graphics::kDawnBootstrapAbiVersion,
             .callback = &install_vellum_override,
             .context = &context},
            header, &coordinator_error);
    } else {
        registered = vellum::app_host::register_native_dawn_bootstrap(&coordinator_error);
    }
    if (!registered || !vellum::graphics::dawn_bootstrap_is_registered(&coordinator_error)) {
        reason = coordinator_error.empty() ? "vellum_dawn_bootstrap_failed" : coordinator_error;
        return false;
    }

    auto& bootstrap = proc_bootstrap();
    std::lock_guard lock(bootstrap.mutex);
    if (bootstrap.installs != 0) {
        if (bootstrap.revision != header) {
            reason = "second_provider_identity_mismatch";
            return false;
        }
        return true;
    }
    bootstrap.revision = header;
    bootstrap.installs = 1;
    return true;
#else
    const auto header = revision(dawn::kDawnVersion.data());
    const auto proc = revision(dawnProcGetVersion());
    const DawnProcTable& native = dawn::native::GetProcs();
    const auto native_revision = revision(native.version);
    if (header.empty() || header != proc || header != native_revision ||
        (!expected.empty() && expected != header)) {
        reason = "provider_identity_mismatch";
        return false;
    }

    auto& bootstrap = proc_bootstrap();
    std::lock_guard lock(bootstrap.mutex);
    if (bootstrap.installs != 0) {
        if (bootstrap.revision != header) {
            reason = "second_provider_identity_mismatch";
            return false;
        }
        return true;
    }
    const auto* selected = override_for_testing == nullptr
                               ? &native
                               : static_cast<const DawnProcTable*>(override_for_testing);
    if (revision(selected->version) != header) {
        reason = "override_provider_identity_mismatch";
        return false;
    }
    dawnProcSetProcs(selected);
    bootstrap.revision = header;
    bootstrap.installs = 1;
    return true;
#endif
}

std::optional<std::size_t> round_up(std::size_t size, std::size_t alignment) {
    if (alignment == 0 || size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        return std::nullopt;
    return ((size + alignment - 1) / alignment) * alignment;
}

std::string copy_string(wgpu::StringView value) {
    if (value.data == nullptr)
        return {};
    const auto length = value.length == wgpu::kStrlen ? std::strlen(value.data) : value.length;
    return {value.data, length};
}

} // namespace

constexpr auto kSharedIoFftWgsl = R"wgsl(
// One radix-2 Stockham auto-sort FFT pass over interleaved complex data.
// Result is naturally ordered (no separate bit-reversal pass). log2(N) passes
// ping-pong between two buffers; `ns` doubles each pass (1, 2, 4, ... N/2).
// sign = -1 forward, +1 inverse (the host applies the 1/N inverse scale).
// Reference: Lloyd/Boyd/Govindaraju, "Fast Computation of General Fourier
// Transforms on GPUs" (Stockham radix-2 formulation).

struct FftParams {
    n     : u32,
    ns    : u32,
    sign  : f32,
    batch : u32,   // number of independent transforms packed back-to-back
};

@group(0) @binding(0) var<storage, read>       src    : array<f32>;
@group(0) @binding(1) var<storage, read_write> dst    : array<f32>;
@group(0) @binding(2) var<uniform>             params : FftParams;

const PI : f32 = 3.1415926535897932;

// Batched radix-2 Stockham pass. Each of `batch` transforms occupies a
// contiguous n-complex span; thread tid maps to (transform b, butterfly j).
// batch == 1 is bit-for-bit identical to the single-transform path.
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let half = params.n / 2u;
    let tid = gid.x;
    if (tid >= params.batch * half) {
        return;
    }
    let b = tid / half;
    let j = tid - b * half;
    let base = b * params.n;   // complex-element offset of this transform

    let v0_re = src[2u * (base + j)];
    let v0_im = src[2u * (base + j) + 1u];
    let k = j + half;
    let v1_re = src[2u * (base + k)];
    let v1_im = src[2u * (base + k) + 1u];

    let angle = params.sign * 2.0 * PI * f32(j % params.ns) / f32(params.ns * 2u);
    let tw_re = cos(angle);
    let tw_im = sin(angle);

    // u1 = twiddle * v1
    let u1_re = tw_re * v1_re - tw_im * v1_im;
    let u1_im = tw_re * v1_im + tw_im * v1_re;

    let y0_re = v0_re + u1_re;
    let y0_im = v0_im + u1_im;
    let y1_re = v0_re - u1_re;
    let y1_im = v0_im - u1_im;

    let idxD = (j / params.ns) * params.ns * 2u + (j % params.ns);
    dst[2u * (base + idxD)]                   = y0_re;
    dst[2u * (base + idxD) + 1u]              = y0_im;
    dst[2u * (base + idxD + params.ns)]       = y1_re;
    dst[2u * (base + idxD + params.ns) + 1u]  = y1_im;
}
)wgsl";

// The first authenticated WaveNet slice deliberately supports one array and
// one layer.  It uses the same flat weight order as render::GpuCompute and is
// enough to prove persistent weights, causal history, and imported I/O buffers
// through the shared-I/O provider before widening the shape.
constexpr auto kSharedIoWavenetRechannelWgsl = R"wgsl(
struct P { C:u32, B:u32, pad:u32, woff:u32 };
@group(0) @binding(0) var<storage, read> wts:array<f32>;
@group(0) @binding(1) var<storage, read> src:array<f32>;
@group(0) @binding(2) var<storage, read_write> dst:array<f32>;
@group(0) @binding(3) var<uniform> p:P;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3u) {
 let t=id.x; if(t>=p.B){return;} dst[p.pad+t]=wts[p.woff]*src[t];
}
)wgsl";
constexpr auto kSharedIoWavenetLayerWgsl = R"wgsl(
struct P { C:u32, K:u32, B:u32, dil:u32, Z:u32, gated:u32, pad:u32, conv_w:u32, conv_b:u32, mix_w:u32, l1_w:u32, l1_b:u32, p0:u32, p1:u32, p2:u32, p3:u32 };
@group(0) @binding(0) var<storage, read> wts:array<f32>;
@group(0) @binding(1) var<storage, read> ina:array<f32>;
@group(0) @binding(2) var<storage, read_write> outa:array<f32>;
@group(0) @binding(3) var<storage, read> cond:array<f32>;
@group(0) @binding(4) var<storage, read_write> headacc:array<f32>;
@group(0) @binding(5) var<uniform> p:P;
var<workgroup> zbuf:array<f32,128>; var<workgroup> abuf:array<f32,64>;
@compute @workgroup_size(64) fn main(@builtin(workgroup_id) wid:vec3u,@builtin(local_invocation_id) lid:vec3u){
 let t=wid.x; if(t>=p.B){return;} let lane=lid.x; let C=p.C; let Z=p.Z; let acol=p.pad+t;
 for(var oc=lane;oc<Z;oc+=64u){var acc=wts[p.conv_b+oc]; for(var k=0u;k<p.K;k++){let back=p.dil*(p.K-1u-k);let base=(acol-back)*C;let wb=p.conv_w+oc*C*p.K+k;for(var ic=0u;ic<C;ic++){acc+=wts[wb+ic*p.K]*ina[base+ic];}} acc+=wts[p.mix_w+oc]*cond[t];zbuf[oc]=acc;}
 workgroupBarrier(); if(p.gated==0u){for(var c=lane;c<C;c+=64u){abuf[c]=tanh(zbuf[c]);}}else{for(var c=lane;c<C;c+=64u){let g=1.0/(1.0+exp(-zbuf[C+c]));abuf[c]=tanh(zbuf[c])*g;}}
 workgroupBarrier(); let hc=t*C; for(var c=lane;c<C;c+=64u){headacc[hc+c]=headacc[hc+c]+abuf[c];} let tc=acol*C; for(var oc=lane;oc<C;oc+=64u){var r=wts[p.l1_b+oc];let rw=p.l1_w+oc*C;for(var ic=0u;ic<C;ic++){r+=wts[rw+ic]*abuf[ic];}outa[tc+oc]=ina[tc+oc]+r;}
}
)wgsl";
constexpr auto kSharedIoWavenetHeadWgsl = R"wgsl(
struct P { C:u32,H:u32,B:u32,hr_w:u32,hr_b:u32,bias:u32,p0:u32,p1:u32 };
@group(0) @binding(0) var<storage,read> wts:array<f32>; @group(0) @binding(1) var<storage,read> acc:array<f32>; @group(0) @binding(2) var<storage,read_write> outp:array<f32>; @group(0) @binding(3) var<uniform> p:P;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3u){let t=id.x;if(t>=p.B){return;}for(var oc=0u;oc<p.H;oc++){var v=0.0;if(p.bias==1u){v=wts[p.hr_b+oc];}for(var ic=0u;ic<p.C;ic++){v+=wts[p.hr_w+oc*p.C+ic]*acc[t*p.C+ic];}outp[t*p.H+oc]=v;}}
)wgsl";
constexpr auto kSharedIoWavenetScaleWgsl = R"wgsl(
struct P { B:u32,H:u32,scale:f32,p0:u32 }; @group(0) @binding(0) var<storage,read> src:array<f32>; @group(0) @binding(1) var<storage,read_write> dst:array<f32>; @group(0) @binding(2) var<uniform> p:P;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3u){let t=id.x;if(t<p.B){dst[t]=src[t*p.H]*p.scale;}}
)wgsl";

constexpr auto kSharedIoMulWgsl = R"wgsl(
@group(0) @binding(0) var<storage,read> a:array<f32>;
@group(0) @binding(1) var<storage,read> ir:array<f32>;
@group(0) @binding(2) var<storage,read_write> out:array<f32>;
@compute @workgroup_size(256) fn main(@builtin(global_invocation_id) id:vec3u) { let i=id.x; let p=i*2u; if(p+1u>=arrayLength(&a)){return;} let k=p%(arrayLength(&ir)); let ar=a[p];let ai=a[p+1u];let br=ir[k];let bi=ir[k+1u];out[p]=ar*br-ai*bi;out[p+1u]=ar*bi+ai*br; })wgsl";

struct DawnSharedIoProvider::Impl {
    struct RequestAdapter {
        std::atomic<bool> done{false};
        wgpu::RequestAdapterStatus status = wgpu::RequestAdapterStatus::Error;
        wgpu::Adapter adapter;
    };
    struct RequestDevice {
        std::atomic<bool> done{false};
        wgpu::RequestDeviceStatus status = wgpu::RequestDeviceStatus::Error;
        wgpu::Device device;
    };
    struct ErrorScope {
        std::atomic<bool> done{false};
        wgpu::PopErrorScopeStatus status = wgpu::PopErrorScopeStatus::Error;
        wgpu::ErrorType type = wgpu::ErrorType::Unknown;
    };
    struct Disposal {
        std::atomic<bool> observed{false};
    };
    struct Submission {
        struct ScopeCallback {
            Submission* submission = nullptr;
            unsigned index = 0;
        };

        std::atomic<int> queue{0};
        std::atomic<int> scope{0};
        std::atomic<unsigned> scopes_pending{0};
        std::atomic<bool> scope_error{false};
        std::array<std::atomic<bool>, 3> scope_completed{};
        DawnSubmissionTracker tracker;
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
        // Future ids are retained until their callbacks have been consumed.
        // Dawn callbacks may be delivered by WaitAny or ProcessEvents, and a
        // stack-local Future would otherwise leave the dispatcher with no
        // safe way to wait for the exact submission it owns.
        wgpu::Future queue_future{};
        std::array<wgpu::Future, 3> scope_futures{};
        std::array<ScopeCallback, 3> scope_callbacks{};
        std::optional<SharedIoTerminalStatus> pending_terminal;
        std::uint64_t generation = 0;
        bool queue_consumed = false;
        bool scope_consumed = false;
        bool accepted = false;
        int forced_queue_result = 0;
        std::optional<SharedIoTerminalInbox::CompletionClaim> held_busy_claim;
        bool held_busy_released = false;
    };
    struct Slot {
        std::uint32_t index = 0;
        void* input = nullptr;
        void* output = nullptr;
        std::size_t input_bytes = 0;
        std::size_t output_bytes = 0;
        std::size_t input_logical_bytes = 0;
        std::size_t output_logical_bytes = 0;
        Disposal input_disposal;
        Disposal output_disposal;
        wgpu::Buffer input_buffer;
        wgpu::Buffer output_buffer;
        bool input_disposal_expected = false;
        bool output_disposal_expected = false;
        std::uint64_t handle_generation = 1;
        wgpu::BindGroup bind_group;
        Submission submission;
        bool retired = false;
    };

    struct ConvolutionPlan {
        std::uint32_t fft_size = 0, channels = 0, logical_frames = 0, log2 = 0;
        std::size_t bytes = 0;
        wgpu::ComputePipeline fft, multiply;
        wgpu::Buffer ir;
        std::vector<wgpu::Buffer> forward_uniforms, inverse_uniforms;
        struct SlotGroups {
            // Submissions from different arena slots may overlap. Keep every
            // writable FFT intermediate slot-local; only immutable IR and
            // uniforms are shared by the prepared program.
            wgpu::Buffer a, b, product;
            std::vector<wgpu::BindGroup> forward, inverse;
            wgpu::BindGroup multiply;
        };
        std::vector<SlotGroups> slots;
    };
    std::unique_ptr<ConvolutionPlan> convolution;
    struct WavenetPlan {
        std::uint32_t block_size = 0, channels = 0, kernel = 0, dilation = 1, pad = 0;
        std::uint32_t gated = 0, head_size = 0, head_bias = 0;
        wgpu::ComputePipeline rechannel, layer, head, scale;
        wgpu::Buffer weights, rechannel_u, layer_u, head_u, scale_u;
        struct SlotGroups {
            wgpu::Buffer act0, act1, headacc, headout, history_temp;
            wgpu::BindGroup rechannel, layer, head, scale;
        };
        std::vector<SlotGroups> slots;
    };
    std::unique_ptr<WavenetPlan> wavenet;
    CompletionPolicy completion_policy = CompletionPolicy::ProcessEvents;
    std::uint64_t completion_wait_ns = 0;
    std::size_t wait_any_batch_cursor = 0;
    bool wait_any_disabled = false;
    bool wait_any_fault_consumed = false;

    explicit Impl(Options value) : options(std::move(value)) {
        completion_policy = options.completion_policy;
        completion_wait_ns = options.completion_wait_ns;
    }

    bool pump_until(const auto& done, std::chrono::steady_clock::time_point deadline) noexcept {
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            instance.ProcessEvents();
            ++stats.process_events_calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }

    static wgpu::CallbackMode callback_mode() noexcept {
        // AllowProcessEvents is deliberate.  Dawn native providers still
        // deliver these callbacks from ProcessEvents; WaitAny is an additional
        // wake-up policy, not a license to use arbitrary-thread callbacks.
        return wgpu::CallbackMode::AllowProcessEvents;
    }

    void process_events() noexcept {
        instance.ProcessEvents();
        ++stats.process_events_calls;
    }

    wgpu::WaitStatus invoke_wait_any(std::size_t count, std::uint64_t timeout_ns) noexcept {
        if (!wait_any_fault_consumed && (options.fault == Fault::SyntheticWaitAnyTimeout ||
                                         options.fault == Fault::SyntheticWaitAnyError)) {
            wait_any_fault_consumed = true;
            ++stats.fault_injections;
            return options.fault == Fault::SyntheticWaitAnyTimeout ? wgpu::WaitStatus::TimedOut
                                                                   : wgpu::WaitStatus::Error;
        }
        return instance.WaitAny(count, wait_infos.data(), timeout_ns);
    }

    bool wait_for_queue_callbacks(std::chrono::steady_clock::time_point deadline) noexcept {
        if (completion_policy == CompletionPolicy::ProcessEvents) {
            process_events();
            return true;
        }
        if (wait_any_disabled) {
            process_events();
            return true;
        }

        const auto batch_count =
            std::max<std::size_t>(1, (slots.size() + kWaitAnyBatchSize - 1) / kWaitAnyBatchSize);
        const auto starting_batch = wait_any_batch_cursor % batch_count;
        std::size_t selected_batch = starting_batch;
        std::size_t wait_count = 0;
        for (std::size_t offset = 0; offset < batch_count; ++offset) {
            const auto batch = (starting_batch + offset) % batch_count;
            const auto first_slot = batch * kWaitAnyBatchSize;
            const auto last_slot = std::min(slots.size(), first_slot + kWaitAnyBatchSize);
            wait_count = 0;
            for (auto slot_index = first_slot; slot_index < last_slot; ++slot_index) {
                const auto* slot = slots[slot_index];
                if (slot == nullptr || !slot->submission.accepted ||
                    slot->submission.queue.load(std::memory_order_acquire) != 0 ||
                    slot->submission.queue_future.id == 0)
                    continue;
                wait_infos[wait_count++] = {slot->submission.queue_future, false};
            }
            if (wait_count != 0) {
                selected_batch = batch;
                break;
            }
        }
        // A device-loss callback remains AllowProcessEvents and is flushed
        // below.  It is intentionally not included in the wait set: keeping
        // queue futures homogeneous avoids Dawn's mixed-source timed-wait
        // restriction, while the explicit ProcessEvents call still accounts
        // for loss delivery before terminal state is observed.
        if (wait_count != 0) {
            std::uint64_t timeout_ns = 0;
            if (completion_policy == CompletionPolicy::TimedWaitAny) {
                const auto now = std::chrono::steady_clock::now();
                // Dawn interprets timeoutNS == 0 as an explicit nonblocking
                // poll. A dispatcher poll therefore remains bounded.
                if (now < deadline) {
                    timeout_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
                            .count());
                    const auto configured =
                        completion_wait_ns == 0 ? kDefaultCompletionWaitNs : completion_wait_ns;
                    timeout_ns =
                        std::min(timeout_ns, std::min(configured, kDefaultCompletionWaitNs));
                }
            }
            ++stats.wait_any_calls;
            stats.wait_any_timed_calls += static_cast<std::uint64_t>(timeout_ns != 0);
            stats.wait_any_max_futures =
                std::max(stats.wait_any_max_futures, static_cast<std::uint64_t>(wait_count));
            stats.wait_any_max_timeout_ns = std::max(stats.wait_any_max_timeout_ns, timeout_ns);
            const auto status = invoke_wait_any(wait_count, timeout_ns);
            if (status == wgpu::WaitStatus::TimedOut) {
                ++stats.wait_any_timeouts;
            } else if (status == wgpu::WaitStatus::Error) {
                // Error is distinct from unsupported/count failure. Keep
                // servicing callbacks so a transient dispatcher error cannot
                // strand an otherwise recoverable submission.
                ++stats.wait_any_errors;
            } else if (status != wgpu::WaitStatus::Success) {
                ++stats.wait_any_unsupported;
                wait_any_disabled = true;
            }
            wait_any_batch_cursor = (selected_batch + 1) % batch_count;
        }

        // PopErrorScope and device-lost callbacks use AllowProcessEvents too.
        // Always pump once after WaitAny so a queue wake cannot be mistaken for
        // a clean submission while its validation scopes or loss callback are
        // still pending.
        process_events();
        return true;
    }

    bool pump_until_completion(const auto& done,
                               std::chrono::steady_clock::time_point deadline) noexcept {
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            if (!wait_for_queue_callbacks(deadline))
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }

    static void dispose_callback(void* userdata) {
        static_cast<Disposal*>(userdata)->observed.store(true, std::memory_order_release);
    }

    void push_error_scopes() noexcept {
        device.PushErrorScope(wgpu::ErrorFilter::Internal);
        device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
        device.PushErrorScope(wgpu::ErrorFilter::Validation);
    }

    bool pop_error_scopes() noexcept {
        bool clean = true;
        for (unsigned index = 0; index < 3; ++index) {
            auto result = std::make_shared<ErrorScope>();
            device.PopErrorScope(
                wgpu::CallbackMode::AllowProcessEvents,
                [result](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
                    result->status = status;
                    result->type = type;
                    result->done.store(true, std::memory_order_release);
                });
            const auto deadline = std::chrono::steady_clock::now() + kDrainLimit;
            clean = pump_until([&] { return result->done.load(std::memory_order_acquire); },
                               deadline) &&
                    result->status == wgpu::PopErrorScopeStatus::Success &&
                    result->type == wgpu::ErrorType::NoError && clean;
        }
        return clean;
    }

    bool initialize(std::string& reason, Availability& availability) {
        if (!install_exact_proc_table(options.expected_dawn_revision,
                                      options.proc_table_override_for_testing, reason)) {
            availability = Availability::InvalidProvider;
            return false;
        }

        wgpu::InstanceDescriptor instance_descriptor{};
        wgpu::InstanceFeatureName instance_features[] = {
            wgpu::InstanceFeatureName::TimedWaitAny,
        };
        if (completion_policy == CompletionPolicy::TimedWaitAny) {
            instance_descriptor.requiredFeatureCount = 1;
            instance_descriptor.requiredFeatures = instance_features;
        }
        native_instance = std::make_unique<dawn::native::Instance>(
            reinterpret_cast<const WGPUInstanceDescriptor*>(&instance_descriptor));
        instance = wgpu::Instance(native_instance->Get());
        if (!instance) {
            reason = "instance_creation_failed";
            return false;
        }

        const char* unsafe_toggle[] = {"allow_unsafe_apis"};
        wgpu::DawnTogglesDescriptor adapter_toggles{};
        adapter_toggles.enabledToggleCount = 1;
        adapter_toggles.enabledToggles = unsafe_toggle;
        wgpu::RequestAdapterOptions adapter_options{};
        adapter_options.powerPreference = wgpu::PowerPreference::HighPerformance;
        adapter_options.nextInChain = &adapter_toggles;
        auto adapter_request = std::make_shared<RequestAdapter>();
        instance.RequestAdapter(&adapter_options, wgpu::CallbackMode::AllowProcessEvents,
                                [adapter_request](wgpu::RequestAdapterStatus status,
                                                  wgpu::Adapter result, wgpu::StringView) {
                                    adapter_request->status = status;
                                    if (status == wgpu::RequestAdapterStatus::Success)
                                        adapter_request->adapter = std::move(result);
                                    adapter_request->done.store(true, std::memory_order_release);
                                });
        const auto deadline = std::chrono::steady_clock::now() + kDrainLimit;
        if (!pump_until([&] { return adapter_request->done.load(std::memory_order_acquire); },
                        deadline) ||
            adapter_request->status != wgpu::RequestAdapterStatus::Success ||
            !adapter_request->adapter) {
            reason = "no_adapter";
            availability = Availability::Unsupported;
            return false;
        }
        adapter = std::move(adapter_request->adapter);
        wgpu::AdapterInfo adapter_info{};
        if (adapter.GetInfo(&adapter_info) != wgpu::Status::Success ||
            adapter_info.backendType != wgpu::BackendType::Metal ||
            adapter_info.vendorID != 0x106b) {
            reason = "apple_metal_adapter_required";
            availability = Availability::Unsupported;
            return false;
        }
        adapter_identity.name = copy_string(adapter_info.device);
        adapter_identity.vendor_id = adapter_info.vendorID;
        adapter_identity.device_id = adapter_info.deviceID;
        if (!adapter.HasFeature(wgpu::FeatureName::HostMappedPointer)) {
            reason = "host_mapped_pointer_unavailable";
            availability = Availability::Unsupported;
            return false;
        }
        wgpu::DawnHostMappedPointerLimits host_limits{};
        wgpu::Limits limits{};
        limits.nextInChain = &host_limits;
        if (adapter.GetLimits(&limits) != wgpu::Status::Success ||
            host_limits.hostMappedPointerAlignment == 0 ||
            host_limits.hostMappedPointerAlignment == wgpu::kLimitU32Undefined ||
            host_limits.hostMappedPointerAlignment < sizeof(void*) ||
            (host_limits.hostMappedPointerAlignment &
             (host_limits.hostMappedPointerAlignment - 1)) != 0) {
            reason = "host_pointer_alignment_unavailable";
            return false;
        }
        alignment = host_limits.hostMappedPointerAlignment;

        wgpu::DawnTogglesDescriptor device_toggles{};
        device_toggles.enabledToggleCount = 1;
        device_toggles.enabledToggles = unsafe_toggle;
        wgpu::FeatureName required[] = {wgpu::FeatureName::HostMappedPointer};
        wgpu::DeviceDescriptor device_descriptor{};
        device_descriptor.label = "Pulp shared GPU audio P1 device";
        device_descriptor.nextInChain = &device_toggles;
        device_descriptor.requiredFeatureCount = 1;
        device_descriptor.requiredFeatures = required;
        device_descriptor.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView,
               std::atomic<std::uint64_t>* generation) {
                generation->fetch_add(1, std::memory_order_release);
            },
            &uncaptured_error_generation);
        device_descriptor.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowProcessEvents,
            [](const wgpu::Device&, wgpu::DeviceLostReason, wgpu::StringView,
               std::atomic<bool>* lost) { lost->store(true, std::memory_order_release); },
            &device_lost);
        auto device_request = std::make_shared<RequestDevice>();
        adapter.RequestDevice(&device_descriptor, wgpu::CallbackMode::AllowProcessEvents,
                              [device_request](wgpu::RequestDeviceStatus status,
                                               wgpu::Device result, wgpu::StringView) {
                                  device_request->status = status;
                                  if (status == wgpu::RequestDeviceStatus::Success)
                                      device_request->device = std::move(result);
                                  device_request->done.store(true, std::memory_order_release);
                              });
        const auto device_deadline = std::chrono::steady_clock::now() + kDrainLimit;
        if (!pump_until([&] { return device_request->done.load(std::memory_order_acquire); },
                        device_deadline) ||
            device_request->status != wgpu::RequestDeviceStatus::Success ||
            !device_request->device) {
            reason = "device_creation_failed";
            return false;
        }
        device = std::move(device_request->device);
        queue = device.GetQueue();

        constexpr auto good_shader = R"wgsl(
@group(0) @binding(0) var<storage, read> input_data : array<f32>;
@group(0) @binding(1) var<storage, read_write> output_data : array<f32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    if (gid.x < arrayLength(&input_data) && gid.x < arrayLength(&output_data)) {
        output_data[gid.x] = input_data[gid.x] * 1.75 + 0.25;
    }
}
)wgsl";
        constexpr auto wrong_shader = R"wgsl(
@group(0) @binding(0) var<storage, read> input_data : array<f32>;
@group(0) @binding(1) var<storage, read_write> output_data : array<f32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    if (gid.x < arrayLength(&input_data) && gid.x < arrayLength(&output_data)) {
        output_data[gid.x] = input_data[gid.x] * 1.75 + 0.5;
    }
}
)wgsl";
        wgpu::ShaderSourceWGSL source{};
        source.code = options.fault == Fault::WrongOutput ? wrong_shader : good_shader;
        wgpu::ShaderModuleDescriptor shader_descriptor{};
        shader_descriptor.nextInChain = &source;
        push_error_scopes();
        const auto module = device.CreateShaderModule(&shader_descriptor);
        wgpu::ComputePipelineDescriptor pipeline_descriptor{};
        pipeline_descriptor.compute.module = module;
        pipeline_descriptor.compute.entryPoint = "main";
        pipeline = device.CreateComputePipeline(&pipeline_descriptor);
        if (!pop_error_scopes() || !module || !pipeline) {
            reason = "pipeline_creation_failed";
            return false;
        }
        accepting = true;
        availability = Availability::Ready;
        return true;
    }

    void refresh(Slot& slot, bool physically_drained) noexcept {
        auto& submission = slot.submission;
        if (!submission.accepted)
            return;
        const auto queue_value = submission.queue.load(std::memory_order_acquire);
        if (!submission.queue_consumed && queue_value != 0) {
            const auto result = queue_value == 1   ? DawnSubmissionTracker::QueueResult::Success
                                : queue_value == 2 ? DawnSubmissionTracker::QueueResult::Cancelled
                                                   : DawnSubmissionTracker::QueueResult::Error;
            submission.tracker.record_queue(submission.generation, result);
            submission.queue_consumed = true;
            submission.queue_future = {};
        }
        const auto scope_value = submission.scope.load(std::memory_order_acquire);
        if (!submission.scope_consumed && scope_value != 0) {
            submission.tracker.record_scope(submission.generation,
                                            scope_value == 1
                                                ? DawnSubmissionTracker::ScopeResult::Clean
                                                : DawnSubmissionTracker::ScopeResult::Error);
            submission.scope_consumed = true;
            for (auto& future : submission.scope_futures)
                future = {};
        }
        if (!submission.pending_terminal) {
            // Every provider operation is enclosed by validation, OOM, and
            // internal error scopes. A clean set of PopErrorScope callbacks
            // therefore rules out a later uncaptured callback for this
            // submission; the generation below covers genuinely out-of-scope
            // device errors. IsDeviceLost closes callback-delivery lag for the
            // native loss state before readable success is published.
            const auto terminal = submission.tracker.observe(
                submission.generation,
                {.uncaptured_error_generation =
                     uncaptured_error_generation.load(std::memory_order_acquire),
                 .device_lost = device_lost.load(std::memory_order_acquire) ||
                                dawn::native::IsDeviceLost(device.Get()),
                 .physically_drained = physically_drained});
            if (terminal) {
                submission.pending_terminal =
                    *terminal == DawnSubmissionTracker::Terminal::RetiredSuccess
                        ? SharedIoTerminalStatus::RetiredSuccess
                        : SharedIoTerminalStatus::RetiredFailed;
            }
        }
        if (submission.pending_terminal) {
            if (options.fault == Fault::HoldTerminalBusy && !submission.held_busy_released) {
                if (!submission.held_busy_claim) {
                    auto attempt = submission.inbox->try_claim(submission.token.slot);
                    submission.held_busy_claim = attempt.claim;
                    if (submission.held_busy_claim &&
                        submission.inbox->push(submission.token, *submission.pending_terminal) ==
                            SharedIoTerminalInbox::PushResult::Busy) {
                        ++stats.terminal_busy_retries;
                    }
                    return;
                }
                auto stale = submission.token;
                ++stale.slot_generation;
                submission.inbox->finish_claim(*submission.held_busy_claim, stale,
                                               *submission.pending_terminal);
                submission.held_busy_claim.reset();
                submission.held_busy_released = true;
            }
            const auto result =
                submission.inbox->push(submission.token, *submission.pending_terminal);
            if (result == SharedIoTerminalInbox::PushResult::Accepted) {
                if (*submission.pending_terminal == SharedIoTerminalStatus::RetiredSuccess)
                    ++stats.retired_success;
                else
                    ++stats.retired_failure;
                submission.accepted = false;
                submission.pending_terminal.reset();
                submission.inbox.reset();
            } else if (result == SharedIoTerminalInbox::PushResult::Rejected) {
                // An accepted submission owns an exact terminal obligation.
                // Rejection is an invariant failure, not retirement evidence;
                // retain all callback state and make the provider non-reusable.
                reusable = false;
            }
        }
    }

    Options options;
    // Device callbacks retain these addresses. Declare them before every Dawn
    // owner so reverse member destruction releases the device first.
    std::atomic<std::uint64_t> uncaptured_error_generation{0};
    std::atomic<bool> device_lost{false};
    std::unique_ptr<dawn::native::Instance> native_instance;
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::ComputePipeline pipeline;
    std::uint32_t alignment = 0;
    std::vector<Slot*> slots;
    // Keep the dispatcher wait path allocation-free. Dawn's timed wait limit
    // is 64 futures, so larger arenas are serviced in bounded batches.
    std::array<wgpu::FutureWaitInfo, kWaitAnyBatchSize> wait_infos{};
    bool accepting = false;
    bool reusable = true;
    bool device_destroyed = false;
    bool refusal_consumed = false;
    Stats stats;
    AdapterIdentity adapter_identity;
    std::shared_ptr<const void> lifetime = std::make_shared<int>(0);
};

class DawnSharedIoConvolutionProgram final : public SharedIoPreparedProgram {
  public:
    DawnSharedIoConvolutionProgram(DawnSharedIoProvider& provider,
                                   const SharedIoConvolutionProgramSpec& spec)
        : provider_(&provider), fft_size_(spec.fft_size), channels_(spec.channels),
          logical_frames_(spec.logical_frames), ir_length_(spec.ir_length),
          normalized_ir_spectrum_(spec.normalized_ir_spectrum.begin(),
                                  spec.normalized_ir_spectrum.end()) {}

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override {
        if (&provider != provider_ || prepared_ || slots.empty())
            return false;
        for (const auto& slot : slots) {
            if (slot.provider != provider_ || !provider_->validate_slot_buffers(slot))
                return false;
        }
        const SharedIoConvolutionProgramSpec spec{
            .fft_size = fft_size_,
            .channels = channels_,
            .logical_frames = logical_frames_,
            .ir_length = ir_length_,
            .normalized_ir_spectrum = normalized_ir_spectrum_,
        };
        prepared_ = provider_->prepare_convolution_program(spec);
        return prepared_;
    }

    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept override {
        return prepared_ && &provider == provider_ &&
               provider_->submit_convolution_program(resources, token, std::move(terminal_inbox));
    }

    bool release() noexcept override {
        if (!prepared_)
            return true;
        if (!provider_->release_convolution_program())
            return false;
        prepared_ = false;
        return true;
    }

  private:
    DawnSharedIoProvider* provider_ = nullptr;
    std::uint32_t fft_size_ = 0;
    std::uint32_t channels_ = 0;
    std::uint32_t logical_frames_ = 0;
    std::uint32_t ir_length_ = 0;
    std::vector<float> normalized_ir_spectrum_;
    bool prepared_ = false;
};

DawnSharedIoProvider::DawnSharedIoProvider(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

DawnSharedIoProvider::~DawnSharedIoProvider() {
    if (!impl_ || impl_->slots.empty())
        return;
    if (!drain()) {
        // A failed terminal barrier cannot justify destroying callback userdata.
        // Leak the complete provider state, including Dawn owners and imported
        // pages, rather than permit a late callback or GPU access to hit freed
        // storage. The arena contract makes this an invariant-failure fallback.
        impl_.release();
        return;
    }
    if (!release_convolution_program()) {
        impl_.release();
        return;
    }
    if (!release_wavenet_program()) {
        impl_.release();
        return;
    }
    for (auto* slot : impl_->slots) {
        SlotResources resources;
        resources.opaque = slot;
        retire_slot(resources);
    }
    if (!drain()) {
        impl_.release();
        return;
    }
    while (!impl_->slots.empty()) {
        SlotResources resources;
        resources.opaque = impl_->slots.back();
        resources.input_lifecycle.allocated = resources.opaque != nullptr;
        resources.output_lifecycle.allocated = resources.opaque != nullptr;
        destroy_slot(resources);
        if (resources.opaque != nullptr) {
            impl_.release();
            return;
        }
    }
}

DawnSharedIoProvider::CreateResult DawnSharedIoProvider::create(const Options& options) noexcept {
    CreateResult result;
    if (options.completion_wait_ns > kMaxCompletionWaitNs) {
        result.availability = Availability::Failed;
        result.reason = "completion_wait_ns_out_of_range";
        return result;
    }
    try {
        auto impl = std::make_unique<Impl>(options);
        if (!impl->initialize(result.reason, result.availability))
            return result;
        result.provider =
            std::unique_ptr<DawnSharedIoProvider>(new DawnSharedIoProvider(std::move(impl)));
    } catch (...) {
        result.availability = Availability::Failed;
        result.reason = "provider_initialization_exception";
    }
    return result;
}

std::unique_ptr<SharedIoPreparedProgram> DawnSharedIoProvider::make_convolution_program(
    const SharedIoConvolutionProgramSpec& spec) noexcept {
    try {
        return std::make_unique<DawnSharedIoConvolutionProgram>(*this, spec);
    } catch (...) {
        return {};
    }
}

std::unique_ptr<SharedIoPreparedProgram>
DawnSharedIoProvider::make_wavenet_program(const DawnSharedIoWavenetProgramSpec& spec) noexcept {
    // The submit token currently carries no stream-instance identity. Refuse
    // multi-instance plans rather than risking causal-history aliasing.
    if (spec.stream_instances != 1)
        return {};
    try {
        return DawnSharedIoWavenetProgram::create(*this, spec);
    } catch (...) {
        return {};
    }
}

DawnSharedIoConvolutionSessionCreateResult create_dawn_shared_io_convolution_session(
    const DawnSharedIoConvolutionSessionOptions& options) noexcept {
    DawnSharedIoConvolutionSessionCreateResult result;
    try {
        auto created = DawnSharedIoProvider::create(options.provider);
        result.availability = created.availability;
        if (!created.provider) {
            result.reason = DawnSharedIoConvolutionSessionCreateResult::Reason::ProviderUnavailable;
            return result;
        }

        const auto& config = options.session.pipeline;
        auto program = created.provider->make_convolution_program(
            {.fft_size = config.fft_size,
             .channels = config.channels,
             .logical_frames = config.block_size,
             .ir_length = config.ir_length,
             .normalized_ir_spectrum = options.normalized_ir_spectrum});
        if (!program) {
            result.availability = DawnSharedIoProvider::Availability::Failed;
            result.reason =
                DawnSharedIoConvolutionSessionCreateResult::Reason::ProgramConstructionFailed;
            return result;
        }

        auto session = std::make_unique<SharedIoConvolutionSession>();
        if (!session->prepare({std::move(created.provider), std::move(program)}, options.session)) {
            // Preparation can retain a physically live arena after an
            // unproven drain. Return that owner so the caller can retry its
            // cleanup barrier; destroying it here would violate the provider
            // lifetime transaction.
            result.session = std::move(session);
            result.availability = DawnSharedIoProvider::Availability::Failed;
            result.reason =
                DawnSharedIoConvolutionSessionCreateResult::Reason::SessionPreparationFailed;
            return result;
        }
        result.session = std::move(session);
        result.availability = DawnSharedIoProvider::Availability::Ready;
        result.reason = DawnSharedIoConvolutionSessionCreateResult::Reason::Ready;
    } catch (...) {
        result.availability = DawnSharedIoProvider::Availability::Failed;
        result.reason = DawnSharedIoConvolutionSessionCreateResult::Reason::ConstructionException;
    }
    return result;
}

bool DawnSharedIoProvider::create_slot(std::uint32_t slot_index, std::size_t input_bytes,
                                       std::size_t output_bytes,
                                       SlotResources& resources) noexcept {
    if (!impl_ || !impl_->accepting || input_bytes % sizeof(float) != 0 ||
        output_bytes % sizeof(float) != 0) {
        return false;
    }
    const auto consume_refusal = [&](Fault fault) {
        if (impl_->options.fault != fault || impl_->options.fault_slot != slot_index ||
            impl_->refusal_consumed) {
            return false;
        }
        impl_->refusal_consumed = true;
        return true;
    };
    if (consume_refusal(Fault::RefuseAllocation))
        return false;
    auto slot = std::unique_ptr<Impl::Slot>(new (std::nothrow) Impl::Slot);
    if (!slot)
        return false;
    slot->index = slot_index;
    const auto rounded_input = round_up(input_bytes, impl_->alignment);
    const auto rounded_output = round_up(output_bytes, impl_->alignment);
    if (!rounded_input || !rounded_output)
        return false;
    slot->input_bytes = *rounded_input;
    slot->output_bytes = *rounded_output;
    slot->input_logical_bytes = input_bytes;
    slot->output_logical_bytes = output_bytes;
    if (posix_memalign(&slot->input, impl_->alignment, slot->input_bytes) != 0)
        return false;
    ++impl_->stats.allocations;
    resources.input_lifecycle.allocated = true;
    if (posix_memalign(&slot->output, impl_->alignment, slot->output_bytes) != 0) {
        resources.opaque = slot.release();
        ++impl_->stats.slots_created;
        return false;
    }
    ++impl_->stats.allocations;
    std::memset(slot->input, 0, slot->input_bytes);
    std::memset(slot->output, 0, slot->output_bytes);
    resources.output_lifecycle.allocated = true;
    try {
        impl_->slots.push_back(slot.get());
    } catch (...) {
        std::free(slot->input);
        std::free(slot->output);
        resources = {};
        return false;
    }

    wgpu::BufferHostMappedPointer input_host{};
    input_host.pointer = slot->input;
    input_host.disposeCallback = Impl::dispose_callback;
    input_host.userdata = &slot->input_disposal;
    const bool native_input_oom = consume_refusal(Fault::NativeInputOom);
    impl_->stats.fault_injections += static_cast<std::uint64_t>(native_input_oom);
    wgpu::BufferDescriptor input_descriptor{};
    input_descriptor.label = "Pulp P1 shared input";
    wgpu::DawnFakeBufferOOMForTesting input_oom{};
    input_oom.fakeOOMAtDevice = true;
    input_host.nextInChain = native_input_oom ? &input_oom : nullptr;
    input_descriptor.nextInChain = &input_host;
    input_descriptor.size = slot->input_bytes;
    input_descriptor.usage =
        wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    if (!consume_refusal(Fault::RefuseInputImport)) {
        resources.input_lifecycle.import_attempted = true;
        ++impl_->stats.import_attempts;
        slot->input_disposal_expected = !native_input_oom;
        impl_->push_error_scopes();
        slot->input_buffer = impl_->device.CreateBuffer(&input_descriptor);
        if (!impl_->pop_error_scopes())
            slot->input_buffer = nullptr;
    }
    resources.input_lifecycle.import_succeeded = bool(slot->input_buffer);
    if (slot->input_buffer)
        ++impl_->stats.import_successes;
    if (!slot->input_buffer) {
        resources.opaque = slot.release();
        ++impl_->stats.slots_created;
        return false;
    }

    wgpu::BufferHostMappedPointer output_host{};
    output_host.pointer = slot->output;
    output_host.disposeCallback = Impl::dispose_callback;
    output_host.userdata = &slot->output_disposal;
    const bool native_output_oom = consume_refusal(Fault::NativeOutputOom);
    impl_->stats.fault_injections += static_cast<std::uint64_t>(native_output_oom);
    wgpu::BufferDescriptor output_descriptor{};
    output_descriptor.label = "Pulp P1 shared output";
    wgpu::DawnFakeBufferOOMForTesting output_oom{};
    output_oom.fakeOOMAtDevice = true;
    output_host.nextInChain = native_output_oom ? &output_oom : nullptr;
    output_descriptor.nextInChain = &output_host;
    output_descriptor.size = slot->output_bytes;
    output_descriptor.usage =
        wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    if (!consume_refusal(Fault::RefuseOutputImport)) {
        resources.output_lifecycle.import_attempted = true;
        ++impl_->stats.import_attempts;
        slot->output_disposal_expected = !native_output_oom;
        impl_->push_error_scopes();
        slot->output_buffer = impl_->device.CreateBuffer(&output_descriptor);
        if (!impl_->pop_error_scopes())
            slot->output_buffer = nullptr;
    }
    resources.output_lifecycle.import_succeeded = bool(slot->output_buffer);
    if (slot->output_buffer)
        ++impl_->stats.import_successes;
    if (!slot->output_buffer) {
        resources.opaque = slot.release();
        ++impl_->stats.slots_created;
        return false;
    }

    wgpu::BindGroupEntry entries[2]{};
    entries[0].binding = 0;
    entries[0].buffer = slot->input_buffer;
    entries[0].size = slot->input_logical_bytes;
    entries[1].binding = 1;
    entries[1].buffer = slot->output_buffer;
    entries[1].size = slot->output_logical_bytes;
    wgpu::BindGroupDescriptor bind_group_descriptor{};
    bind_group_descriptor.layout = impl_->pipeline.GetBindGroupLayout(0);
    bind_group_descriptor.entryCount = 2;
    bind_group_descriptor.entries = entries;
    impl_->push_error_scopes();
    slot->bind_group = impl_->device.CreateBindGroup(&bind_group_descriptor);
    if (!impl_->pop_error_scopes() || !slot->bind_group) {
        resources.opaque = slot.release();
        ++impl_->stats.slots_created;
        return false;
    }

    resources.input = static_cast<std::byte*>(slot->input);
    resources.input_size = input_bytes;
    resources.output = static_cast<std::byte*>(slot->output);
    resources.output_size = output_bytes;
    resources.opaque = slot.get();
    ++impl_->stats.slots_created;
    slot.release();
    return true;
}

void DawnSharedIoProvider::retire_slot(SlotResources& resources) noexcept {
    auto* slot = static_cast<Impl::Slot*>(resources.opaque);
    if (!slot || slot->retired)
        return;
    slot->retired = true;
    ++slot->handle_generation;
    slot->bind_group = nullptr;
    if (slot->input_buffer) {
        slot->input_buffer.Destroy();
        slot->input_buffer = nullptr;
    }
    if (slot->output_buffer) {
        slot->output_buffer.Destroy();
        slot->output_buffer = nullptr;
    }
}

void DawnSharedIoProvider::destroy_slot(SlotResources& resources) noexcept {
    auto* slot = static_cast<Impl::Slot*>(resources.opaque);
    if (!slot)
        return;
    resources.input_lifecycle.dispose_observed =
        slot->input_disposal.observed.load(std::memory_order_acquire);
    resources.output_lifecycle.dispose_observed =
        slot->output_disposal.observed.load(std::memory_order_acquire);
    const bool input_safe =
        !slot->input_disposal_expected || resources.input_lifecycle.dispose_observed;
    const bool output_safe =
        !slot->output_disposal_expected || resources.output_lifecycle.dispose_observed;
    if (!slot->retired || !input_safe || !output_safe)
        return;
    impl_->stats.disposals_observed +=
        static_cast<std::uint64_t>(resources.input_lifecycle.dispose_observed) +
        static_cast<std::uint64_t>(resources.output_lifecycle.dispose_observed);
    impl_->stats.host_frees += static_cast<std::uint64_t>(slot->input != nullptr) +
                               static_cast<std::uint64_t>(slot->output != nullptr);
    std::free(slot->input);
    std::free(slot->output);
    resources.input_lifecycle.host_freed = true;
    resources.output_lifecycle.host_freed = true;
    std::erase(impl_->slots, slot);
    delete slot;
    ++impl_->stats.slots_destroyed;
    resources = {};
    if (impl_->slots.empty() && impl_->reusable && !impl_->device_destroyed)
        impl_->accepting = true;
}

bool DawnSharedIoProvider::acquire_slot_buffers(const SlotResources& resources,
                                                SlotBufferHandle& handle) const noexcept {
    auto* slot = static_cast<Impl::Slot*>(resources.opaque);
    if (!impl_ || !slot || slot->retired || !slot->input_buffer || !slot->output_buffer ||
        resources.opaque != slot || resources.input == nullptr || resources.output == nullptr)
        return false;
    handle = {};
    handle.provider = this;
    handle.device = &impl_->device;
    handle.input_buffer = &slot->input_buffer;
    handle.output_buffer = &slot->output_buffer;
    handle.slot = slot->index;
    handle.generation = slot->handle_generation;
    handle.lifetime = impl_->lifetime;
    return true;
}

bool DawnSharedIoProvider::validate_slot_buffers(const SlotBufferHandle& handle) const noexcept {
    if (!impl_ || handle.provider != this || handle.device != &impl_->device ||
        handle.lifetime.expired() || handle.lifetime.lock() != impl_->lifetime)
        return false;
    for (const auto* slot : impl_->slots) {
        if (slot->index == handle.slot && !slot->retired &&
            handle.input_buffer == &slot->input_buffer &&
            handle.output_buffer == &slot->output_buffer &&
            handle.generation == slot->handle_generation)
            return true;
    }
    return false;
}

bool DawnSharedIoProvider::prepare_convolution_program(
    const SharedIoConvolutionProgramSpec& spec) noexcept {
    if (!impl_ || !impl_->accepting || impl_->convolution || spec.fft_size < 2 ||
        (spec.fft_size & (spec.fft_size - 1)) != 0 || spec.channels == 0 ||
        spec.logical_frames == 0 || spec.ir_length == 0 ||
        std::uint64_t(spec.logical_frames) + spec.ir_length - 1u > spec.fft_size ||
        spec.normalized_ir_spectrum.size() != spec.fft_size * 2u) {
        return false;
    }
    const auto bytes64 = std::uint64_t(spec.fft_size) * spec.channels * 2u * sizeof(float);
    if (bytes64 > std::numeric_limits<std::uint32_t>::max())
        return false;
    for (const auto* slot : impl_->slots) {
        if (slot == nullptr || slot->retired || slot->index >= impl_->slots.size() ||
            slot->input_logical_bytes < bytes64 || slot->output_logical_bytes < bytes64) {
            return false;
        }
    }

    bool error_scopes_pushed = false;
    try {
        auto plan = std::make_unique<Impl::ConvolutionPlan>();
        plan->fft_size = spec.fft_size;
        plan->channels = spec.channels;
        plan->logical_frames = spec.logical_frames;
        plan->bytes = static_cast<std::size_t>(bytes64);
        for (auto size = spec.fft_size; size > 1; size >>= 1)
            ++plan->log2;

        auto pipeline = [&](const char* source) {
            wgpu::ShaderSourceWGSL wgsl{};
            wgsl.code = source;
            wgpu::ShaderModuleDescriptor shader_descriptor{};
            shader_descriptor.nextInChain = &wgsl;
            auto module = impl_->device.CreateShaderModule(&shader_descriptor);
            wgpu::ComputePipelineDescriptor pipeline_descriptor{};
            pipeline_descriptor.compute.module = module;
            pipeline_descriptor.compute.entryPoint = "main";
            return impl_->device.CreateComputePipeline(&pipeline_descriptor);
        };

        auto make_buffer = [&](std::size_t bytes, wgpu::BufferUsage usage) {
            wgpu::BufferDescriptor descriptor{};
            descriptor.size = bytes;
            descriptor.usage = usage;
            return impl_->device.CreateBuffer(&descriptor);
        };
        auto bind_three = [&](const wgpu::ComputePipeline& pipeline_value,
                              const wgpu::Buffer& first, const wgpu::Buffer& second,
                              const wgpu::Buffer& third) {
            wgpu::BindGroupEntry entries[3]{};
            for (std::uint32_t index = 0; index < 3; ++index)
                entries[index].binding = index;
            entries[0].buffer = first;
            entries[0].size = first.GetSize();
            entries[1].buffer = second;
            entries[1].size = second.GetSize();
            entries[2].buffer = third;
            entries[2].size = third.GetSize();
            wgpu::BindGroupDescriptor descriptor{};
            descriptor.layout = pipeline_value.GetBindGroupLayout(0);
            descriptor.entryCount = 3;
            descriptor.entries = entries;
            return impl_->device.CreateBindGroup(&descriptor);
        };

        impl_->push_error_scopes();
        error_scopes_pushed = true;
        const auto fail_preparation = [&] {
            if (error_scopes_pushed) {
                (void)impl_->pop_error_scopes();
                error_scopes_pushed = false;
            }
            return false;
        };
        if (impl_->options.fault == Fault::ConvolutionPrepareAfterScopesFailure) {
            ++impl_->stats.fault_injections;
            return fail_preparation();
        }
        plan->fft = pipeline(kSharedIoFftWgsl);
        plan->multiply = pipeline(kSharedIoMulWgsl);
        const auto storage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        plan->ir =
            make_buffer(static_cast<std::size_t>(spec.fft_size) * 2u * sizeof(float), storage);
        if (!plan->fft || !plan->multiply || !plan->ir)
            return fail_preparation();

        impl_->queue.WriteBuffer(plan->ir, 0, spec.normalized_ir_spectrum.data(),
                                 static_cast<std::size_t>(spec.fft_size) * 2u * sizeof(float));
        struct Params {
            std::uint32_t n;
            std::uint32_t ns;
            float sign;
            std::uint32_t batch;
        };
        for (std::uint32_t stage = 0; stage < plan->log2; ++stage) {
            const auto uniform_usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            auto forward = make_buffer(sizeof(Params), uniform_usage);
            auto inverse = make_buffer(sizeof(Params), uniform_usage);
            if (!forward || !inverse)
                return fail_preparation();
            const Params forward_params{plan->fft_size, 1u << stage, -1.0f, plan->channels};
            const Params inverse_params{plan->fft_size, 1u << stage, 1.0f, plan->channels};
            impl_->queue.WriteBuffer(forward, 0, &forward_params, sizeof(forward_params));
            impl_->queue.WriteBuffer(inverse, 0, &inverse_params, sizeof(inverse_params));
            plan->forward_uniforms.push_back(std::move(forward));
            plan->inverse_uniforms.push_back(std::move(inverse));
        }

        plan->slots.resize(impl_->slots.size());
        for (const auto* slot : impl_->slots) {
            auto& groups = plan->slots[slot->index];
            groups.a = make_buffer(plan->bytes, storage);
            groups.b = make_buffer(plan->bytes, storage);
            groups.product = make_buffer(plan->bytes, storage);
            if (!groups.a || !groups.b || !groups.product)
                return fail_preparation();
            wgpu::Buffer source = slot->input_buffer;
            wgpu::Buffer destination = groups.a;
            for (std::uint32_t stage = 0; stage < plan->log2; ++stage) {
                auto group =
                    bind_three(plan->fft, source, destination, plan->forward_uniforms[stage]);
                if (!group)
                    return fail_preparation();
                groups.forward.push_back(std::move(group));
                source = destination;
                destination = destination.Get() == groups.a.Get() ? groups.b : groups.a;
            }
            groups.multiply = bind_three(plan->multiply, source, plan->ir, groups.product);
            if (!groups.multiply)
                return fail_preparation();

            source = groups.product;
            destination = groups.a;
            for (std::uint32_t stage = 0; stage < plan->log2; ++stage) {
                if (stage + 1u == plan->log2)
                    destination = slot->output_buffer;
                auto group =
                    bind_three(plan->fft, source, destination, plan->inverse_uniforms[stage]);
                if (!group)
                    return fail_preparation();
                groups.inverse.push_back(std::move(group));
                source = destination;
                if (stage + 1u < plan->log2)
                    destination = destination.Get() == groups.a.Get() ? groups.b : groups.a;
            }
        }

        const bool scopes_clean = impl_->pop_error_scopes();
        error_scopes_pushed = false;
        if (!scopes_clean)
            return false;
        impl_->convolution = std::move(plan);
        return true;
    } catch (...) {
        if (error_scopes_pushed)
            (void)impl_->pop_error_scopes();
        return false;
    }
}

bool DawnSharedIoProvider::prepare_wavenet_program(
    const DawnSharedIoWavenetProgramSpec& spec,
    std::span<const SlotBufferHandle> handles) noexcept {
    if (!impl_ || !impl_->accepting || impl_->wavenet || handles.empty() ||
        !validate_dawn_shared_io_wavenet_spec(spec).accepted() || spec.arrays.size() != 1 ||
        spec.arrays[0].dilations.size() != 1)
        return false;
    const auto& s = spec.arrays[0];
    const auto pad64 = std::uint64_t(s.kernel - 1u) * s.dilations[0];
    if (pad64 > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto pad = static_cast<std::uint32_t>(pad64);
    const auto C = s.channels;
    const auto H = s.head_size;
    const auto Z = s.gated ? 2u * C : C;
    const auto bytes = static_cast<std::size_t>(C) * (pad + spec.block_size) * sizeof(float);
    try {
        auto plan = std::make_unique<Impl::WavenetPlan>();
        plan->block_size = spec.block_size;
        plan->channels = C;
        plan->kernel = s.kernel;
        plan->dilation = s.dilations[0];
        plan->pad = pad;
        plan->gated = s.gated;
        plan->head_size = H;
        plan->head_bias = s.head_bias;
        auto make_pipeline = [&](const char* source) {
            wgpu::ShaderSourceWGSL wgsl{};
            wgsl.code = source;
            wgpu::ShaderModuleDescriptor sd{};
            sd.nextInChain = &wgsl;
            auto module = impl_->device.CreateShaderModule(&sd);
            wgpu::ComputePipelineDescriptor pd{};
            pd.compute.module = module;
            pd.compute.entryPoint = "main";
            return impl_->device.CreateComputePipeline(&pd);
        };
        auto make_storage = [&](std::size_t size) {
            wgpu::BufferDescriptor d{};
            d.size = size;
            d.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc |
                      wgpu::BufferUsage::CopyDst;
            return impl_->device.CreateBuffer(&d);
        };
        impl_->push_error_scopes();
        struct ErrorScopeGuard {
            Impl* impl;
            bool active = true;
            ~ErrorScopeGuard() {
                if (active)
                    (void)impl->pop_error_scopes();
            }
            bool close() noexcept {
                if (!active)
                    return true;
                active = false;
                return impl->pop_error_scopes();
            }
        } error_scope{impl_.get()};
        plan->rechannel = make_pipeline(kSharedIoWavenetRechannelWgsl);
        plan->layer = make_pipeline(kSharedIoWavenetLayerWgsl);
        plan->head = make_pipeline(kSharedIoWavenetHeadWgsl);
        plan->scale = make_pipeline(kSharedIoWavenetScaleWgsl);
        plan->weights = make_storage(spec.weights.size() * sizeof(float));
        if (!plan->rechannel || !plan->layer || !plan->head || !plan->scale || !plan->weights)
            return false;
        impl_->queue.WriteBuffer(plan->weights, 0, spec.weights.data(),
                                 spec.weights.size() * sizeof(float));
        struct RcU {
            std::uint32_t C, B, pad, woff;
        };
        struct LyU {
            std::uint32_t C, K, B, dil, Z, gated, pad, conv_w, conv_b, mix_w, l1_w, l1_b, p0, p1,
                p2, p3;
        };
        struct HdU {
            std::uint32_t C, H, B, hr_w, hr_b, bias, p0, p1;
        };
        struct ScU {
            std::uint32_t B, H;
            float scale;
            std::uint32_t p0;
        };
        plan->rechannel_u = make_storage(sizeof(RcU));
        plan->layer_u = make_storage(sizeof(LyU));
        plan->head_u = make_storage(sizeof(HdU));
        plan->scale_u = make_storage(sizeof(ScU));
        if (!plan->rechannel_u || !plan->layer_u || !plan->head_u || !plan->scale_u)
            return false;
        const std::uint32_t rc_w = 0;
        const std::uint32_t conv_w = C * s.input_size;
        const std::uint32_t conv_b = conv_w + Z * C * s.kernel;
        const std::uint32_t mix_w = conv_b + Z;
        const std::uint32_t l1_w = mix_w + Z * s.condition_size;
        const std::uint32_t l1_b = l1_w + C * C;
        const std::uint32_t head_w = l1_b + C;
        const std::uint32_t head_b = head_w + H * C;
        const RcU rcu{C, spec.block_size, pad, rc_w};
        const LyU lyu{C,      s.kernel, spec.block_size, s.dilations[0], Z,    s.gated, pad,
                      conv_w, conv_b,   mix_w,           l1_w,           l1_b, 0,       0,
                      0,      0};
        const HdU hdu{C, H, spec.block_size, head_w, head_b, s.head_bias, 0, 0};
        const ScU scu{spec.block_size, H, spec.head_scale, 0};
        const auto uniform_usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        // These buffers are uniform-sized storage allocations solely to keep
        // this first slice simple; Dawn accepts them in a uniform bind slot
        // only when usage includes Uniform, so recreate them with the proper usage.
        auto make_uniform = [&](const void* data, std::size_t size) {
            wgpu::BufferDescriptor d{};
            d.size = size;
            d.usage = uniform_usage;
            auto b = impl_->device.CreateBuffer(&d);
            if (b)
                impl_->queue.WriteBuffer(b, 0, data, size);
            return b;
        };
        plan->rechannel_u = make_uniform(&rcu, sizeof(rcu));
        plan->layer_u = make_uniform(&lyu, sizeof(lyu));
        plan->head_u = make_uniform(&hdu, sizeof(hdu));
        plan->scale_u = make_uniform(&scu, sizeof(scu));
        if (!plan->rechannel_u || !plan->layer_u || !plan->head_u || !plan->scale_u)
            return false;
        plan->slots.resize(impl_->slots.size());
        for (const auto& handle : handles) {
            if (handle.slot >= plan->slots.size() || !validate_slot_buffers(handle))
                return false;
            const auto* in = static_cast<const wgpu::Buffer*>(handle.input_buffer);
            const auto* out = static_cast<const wgpu::Buffer*>(handle.output_buffer);
            if (in == nullptr || out == nullptr)
                return false;
            auto& g = plan->slots[handle.slot];
            g.act0 = make_storage(bytes);
            g.act1 = make_storage(bytes);
            g.headacc = make_storage(static_cast<std::size_t>(C) * spec.block_size * sizeof(float));
            g.headout = make_storage(static_cast<std::size_t>(H) * spec.block_size * sizeof(float));
            g.history_temp = make_storage(
                std::max<std::size_t>(1, static_cast<std::size_t>(C) * pad * sizeof(float)));
            if (!g.act0 || !g.act1 || !g.headacc || !g.headout || !g.history_temp)
                return false;
            if (pad != 0) {
                std::vector<std::byte> zero(bytes);
                impl_->queue.WriteBuffer(g.act0, 0, zero.data(), zero.size());
                impl_->queue.WriteBuffer(g.act1, 0, zero.data(), zero.size());
            }
            auto bind = [&](const wgpu::ComputePipeline& pipeline,
                            const wgpu::BindGroupEntry* entries, std::uint32_t count) {
                wgpu::BindGroupDescriptor d{};
                d.layout = pipeline.GetBindGroupLayout(0);
                d.entryCount = count;
                d.entries = entries;
                return impl_->device.CreateBindGroup(&d);
            };
            wgpu::BindGroupEntry r_entries[4]{};
            r_entries[0].binding = 0;
            r_entries[0].buffer = plan->weights;
            r_entries[0].size = plan->weights.GetSize();
            r_entries[1].binding = 1;
            r_entries[1].buffer = *in;
            r_entries[1].size = spec.block_size * sizeof(float);
            r_entries[2].binding = 2;
            r_entries[2].buffer = g.act0;
            r_entries[2].size = bytes;
            r_entries[3].binding = 3;
            r_entries[3].buffer = plan->rechannel_u;
            r_entries[3].size = sizeof(RcU);
            g.rechannel = bind(plan->rechannel, r_entries, 4);
            wgpu::BindGroupEntry l_entries[6]{};
            l_entries[0].binding = 0;
            l_entries[0].buffer = plan->weights;
            l_entries[0].size = plan->weights.GetSize();
            l_entries[1].binding = 1;
            l_entries[1].buffer = g.act0;
            l_entries[1].size = bytes;
            l_entries[2].binding = 2;
            l_entries[2].buffer = g.act1;
            l_entries[2].size = bytes;
            l_entries[3].binding = 3;
            l_entries[3].buffer = *in;
            l_entries[3].size = spec.block_size * sizeof(float);
            l_entries[4].binding = 4;
            l_entries[4].buffer = g.headacc;
            l_entries[4].size = g.headacc.GetSize();
            l_entries[5].binding = 5;
            l_entries[5].buffer = plan->layer_u;
            l_entries[5].size = sizeof(LyU);
            g.layer = bind(plan->layer, l_entries, 6);
            wgpu::BindGroupEntry h_entries[4]{};
            h_entries[0].binding = 0;
            h_entries[0].buffer = plan->weights;
            h_entries[0].size = plan->weights.GetSize();
            h_entries[1].binding = 1;
            h_entries[1].buffer = g.headacc;
            h_entries[1].size = g.headacc.GetSize();
            h_entries[2].binding = 2;
            h_entries[2].buffer = g.headout;
            h_entries[2].size = g.headout.GetSize();
            h_entries[3].binding = 3;
            h_entries[3].buffer = plan->head_u;
            h_entries[3].size = sizeof(HdU);
            g.head = bind(plan->head, h_entries, 4);
            wgpu::BindGroupEntry s_entries[3]{};
            s_entries[0].binding = 0;
            s_entries[0].buffer = g.headout;
            s_entries[0].size = g.headout.GetSize();
            s_entries[1].binding = 1;
            s_entries[1].buffer = *out;
            s_entries[1].size = spec.block_size * sizeof(float);
            s_entries[2].binding = 2;
            s_entries[2].buffer = plan->scale_u;
            s_entries[2].size = sizeof(ScU);
            g.scale = bind(plan->scale, s_entries, 3);
            if (!g.rechannel || !g.layer || !g.head || !g.scale)
                return false;
        }
        if (!error_scope.close())
            return false;
        impl_->wavenet = std::move(plan);
        return true;
    } catch (...) {
        return false;
    }
}

bool DawnSharedIoProvider::submit_convolution_program(
    const SlotResources& resources, SlotToken token,
    std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept {
    return submit_impl(resources, token, std::move(inbox), 1);
}

bool DawnSharedIoProvider::submit_wavenet_program(
    const SlotResources& resources, SlotToken token,
    std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept {
    return submit_impl(resources, token, std::move(inbox), 2);
}

bool DawnSharedIoProvider::submit(const SlotResources& resources, SlotToken token,
                                  std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept {
    return submit_impl(resources, token, std::move(terminal_inbox), 0);
}

bool DawnSharedIoProvider::release_wavenet_program() noexcept {
    if (!impl_ || !impl_->wavenet)
        return true;
    for (const auto* slot : impl_->slots)
        if (slot->submission.accepted)
            return false;
    impl_->wavenet.reset();
    return true;
}

bool DawnSharedIoProvider::release_convolution_program() noexcept {
    if (!impl_)
        return true;
    for (const auto* slot : impl_->slots) {
        if (slot->submission.accepted)
            return false;
    }
    impl_->convolution.reset();
    return true;
}

bool DawnSharedIoProvider::submit_impl(const SlotResources& resources, SlotToken token,
                                       std::shared_ptr<SharedIoTerminalInbox> terminal_inbox,
                                       unsigned kind) noexcept {
    auto* slot = static_cast<Impl::Slot*>(resources.opaque);
    if (!impl_ || !impl_->accepting || !terminal_inbox || !slot || slot->retired ||
        slot->submission.accepted || resources.input != slot->input ||
        resources.output != slot->output || resources.input_size != slot->input_logical_bytes ||
        resources.output_size != slot->output_logical_bytes ||
        (impl_->options.fault == Fault::RejectBeforeSubmit &&
         impl_->options.fault_slot == slot->index)) {
        return false;
    }

    const bool use_convolution = kind == 1;
    const bool use_wavenet = kind == 2;
    const Impl::ConvolutionPlan* convolution = nullptr;
    const Impl::WavenetPlan* wavenet = nullptr;
    if (use_convolution) {
        convolution = impl_->convolution.get();
        if (convolution == nullptr || slot->index >= convolution->slots.size())
            return false;
        const auto& groups = convolution->slots[slot->index];
        if (groups.forward.size() != convolution->log2 ||
            groups.inverse.size() != convolution->log2 || !groups.multiply) {
            return false;
        }
    } else if (use_wavenet) {
        wavenet = impl_->wavenet.get();
        if (wavenet == nullptr || slot->index >= wavenet->slots.size() ||
            !wavenet->slots[slot->index].rechannel)
            return false;
    } else if (!impl_->pipeline || !slot->bind_group) {
        return false;
    }

    impl_->push_error_scopes();
    if (use_convolution && impl_->options.fault == Fault::ConvolutionSubmitAfterScopesFailure) {
        ++impl_->stats.fault_injections;
        (void)impl_->pop_error_scopes();
        return false;
    }
    if (impl_->options.fault == Fault::ForceLossBeforeSubmit)
        impl_->device.ForceLoss(wgpu::DeviceLostReason::Unknown, "P1 loss before submit");
    if (impl_->options.fault == Fault::PlantWriteBuffer) {
        constexpr float planted = -123.0f;
        impl_->queue.WriteBuffer(slot->input_buffer, 0, &planted, sizeof(planted));
    }
    auto encoder = impl_->device.CreateCommandEncoder();
    if (!encoder) {
        (void)impl_->pop_error_scopes();
        return false;
    }
    if (impl_->options.fault == Fault::PlantCopyBuffer) {
        encoder.CopyBufferToBuffer(slot->input_buffer, 0, slot->output_buffer, 0, sizeof(float));
    }
    if (impl_->options.fault == Fault::PlantMapAsync) {
        wgpu::BufferDescriptor control_descriptor{};
        control_descriptor.size = sizeof(float);
        control_descriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        auto control = impl_->device.CreateBuffer(&control_descriptor);
        control.MapAsync(wgpu::MapMode::Read, 0, sizeof(float),
                         wgpu::CallbackMode::AllowProcessEvents,
                         [](wgpu::MapAsyncStatus, wgpu::StringView) {});
    }
    if (convolution != nullptr) {
        const auto& plan = *convolution;
        const auto& groups = plan.slots[slot->index];
        const auto fft_wg = (plan.channels * (plan.fft_size / 2u) + 255u) / 256u;
        for (const auto& group : groups.forward) {
            auto pass = encoder.BeginComputePass();
            pass.SetPipeline(plan.fft);
            pass.SetBindGroup(0, group);
            pass.DispatchWorkgroups(fft_wg);
            pass.End();
        }
        {
            auto pass = encoder.BeginComputePass();
            pass.SetPipeline(plan.multiply);
            pass.SetBindGroup(0, groups.multiply);
            pass.DispatchWorkgroups((plan.channels * plan.fft_size + 255u) / 256u);
            pass.End();
        }
        for (const auto& group : groups.inverse) {
            auto pass = encoder.BeginComputePass();
            pass.SetPipeline(plan.fft);
            pass.SetBindGroup(0, group);
            pass.DispatchWorkgroups(fft_wg);
            pass.End();
        }
    } else if (wavenet != nullptr) {
        const auto& plan = *wavenet;
        const auto& groups = plan.slots[slot->index];
        encoder.ClearBuffer(groups.headacc, 0, groups.headacc.GetSize());
        auto pass = encoder.BeginComputePass();
        pass.SetPipeline(plan.rechannel);
        pass.SetBindGroup(0, groups.rechannel);
        pass.DispatchWorkgroups((plan.block_size + 63u) / 64u);
        pass.SetPipeline(plan.layer);
        pass.SetBindGroup(0, groups.layer);
        pass.DispatchWorkgroups(plan.block_size);
        pass.SetPipeline(plan.head);
        pass.SetBindGroup(0, groups.head);
        pass.DispatchWorkgroups((plan.block_size + 63u) / 64u);
        pass.End();
        auto scale_pass = encoder.BeginComputePass();
        scale_pass.SetPipeline(plan.scale);
        scale_pass.SetBindGroup(0, groups.scale);
        scale_pass.DispatchWorkgroups((plan.block_size + 63u) / 64u);
        scale_pass.End();
        if (plan.pad != 0) {
            const auto history_bytes =
                static_cast<std::uint64_t>(plan.channels) * plan.pad * sizeof(float);
            const auto tail_offset =
                static_cast<std::uint64_t>(plan.channels) * plan.block_size * sizeof(float);
            encoder.CopyBufferToBuffer(groups.act0, tail_offset, groups.history_temp, 0,
                                       history_bytes);
            encoder.CopyBufferToBuffer(groups.history_temp, 0, groups.act0, 0, history_bytes);
            encoder.CopyBufferToBuffer(groups.act1, tail_offset, groups.history_temp, 0,
                                       history_bytes);
            encoder.CopyBufferToBuffer(groups.history_temp, 0, groups.act1, 0, history_bytes);
        }
    } else {
        auto pass = encoder.BeginComputePass();
        pass.SetPipeline(impl_->pipeline);
        pass.SetBindGroup(0, slot->bind_group);
        const auto samples =
            std::min(slot->input_logical_bytes, slot->output_logical_bytes) / sizeof(float);
        pass.DispatchWorkgroups(static_cast<std::uint32_t>((samples + 63) / 64));
        if (impl_->options.fault == Fault::InvalidCommandAfterSubmit)
            pass.DispatchWorkgroups(std::numeric_limits<std::uint32_t>::max());
        pass.End();
    }
    auto commands = encoder.Finish();
    if (!commands) {
        (void)impl_->pop_error_scopes();
        return false;
    }

    auto& submission = slot->submission;
    ++submission.generation;
    if (!submission.tracker.begin(submission.generation, impl_->uncaptured_error_generation.load(
                                                             std::memory_order_acquire))) {
        (void)impl_->pop_error_scopes();
        return false;
    }
    submission.queue.store(0, std::memory_order_release);
    submission.scope.store(0, std::memory_order_release);
    submission.scopes_pending.store(3, std::memory_order_release);
    submission.scope_error.store(false, std::memory_order_release);
    submission.queue_consumed = false;
    submission.scope_consumed = false;
    for (auto& completed : submission.scope_completed)
        completed.store(false, std::memory_order_release);
    for (auto& future : submission.scope_futures)
        future = {};
    for (unsigned index = 0; index < submission.scope_callbacks.size(); ++index)
        submission.scope_callbacks[index] = {&submission, index};
    submission.token = token;
    submission.inbox = std::move(terminal_inbox);
    submission.pending_terminal.reset();
    submission.forced_queue_result = impl_->options.fault == Fault::SyntheticQueueError       ? 3
                                     : impl_->options.fault == Fault::SyntheticQueueCancelled ? 2
                                                                                              : 0;
    submission.held_busy_claim.reset();
    submission.held_busy_released = false;

    impl_->queue.Submit(1, &commands);
    submission.accepted = true;
    if (impl_->options.fault == Fault::ForceLossBetweenSubmitAndCompletionRegistration)
        impl_->device.ForceLoss(wgpu::DeviceLostReason::Unknown,
                                "P1 loss before completion registration");
    submission.queue_future = impl_->queue.OnSubmittedWorkDone(
        Impl::callback_mode(),
        [](wgpu::QueueWorkDoneStatus status, wgpu::StringView, Impl::Submission* state) {
            const int value = state->forced_queue_result != 0 ? state->forced_queue_result
                              : status == wgpu::QueueWorkDoneStatus::Success           ? 1
                              : status == wgpu::QueueWorkDoneStatus::CallbackCancelled ? 2
                                                                                       : 3;
            state->queue.store(value, std::memory_order_release);
        },
        &submission);
    if (impl_->options.fault == Fault::ForceLossAfterCompletionRegistration)
        impl_->device.ForceLoss(wgpu::DeviceLostReason::Unknown,
                                "P1 loss after completion registration");
    for (unsigned index = 0; index < 3; ++index) {
        submission.scope_futures[index] = impl_->device.PopErrorScope(
            Impl::callback_mode(),
            [](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView,
               Impl::Submission::ScopeCallback* callback) {
                auto* state = callback->submission;
                const bool clean = status == wgpu::PopErrorScopeStatus::Success &&
                                   type == wgpu::ErrorType::NoError;
                if (!clean)
                    state->scope_error.store(true, std::memory_order_release);
                state->scope_completed[callback->index].store(true, std::memory_order_release);
                if (state->scopes_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    state->scope.store(state->scope_error.load(std::memory_order_acquire) ? 2 : 1,
                                       std::memory_order_release);
                }
            },
            &submission.scope_callbacks[index]);
    }
    if (impl_->options.fault == Fault::PoisonAfterSubmit &&
        impl_->options.fault_slot == slot->index) {
        impl_->uncaptured_error_generation.fetch_add(1, std::memory_order_release);
    }
    return true;
}

void DawnSharedIoProvider::poll() noexcept {
    if (!impl_)
        return;
    // poll() is the nonblocking dispatcher hook. Timed waits are reserved for
    // the serialized drain barrier; WaitAny(0) is Dawn's explicit nonblocking
    // path and TimedWaitAny sees an expired deadline here.
    const auto deadline = std::chrono::steady_clock::now();
    (void)impl_->wait_for_queue_callbacks(deadline);
    for (auto* slot : impl_->slots) {
        if (impl_->options.fault == Fault::DelayCompletion &&
            impl_->options.fault_slot == slot->index) {
            continue;
        }
        impl_->refresh(*slot, false);
    }
}

bool DawnSharedIoProvider::drain() noexcept {
    if (!impl_)
        return true;
    impl_->accepting = false;
    ++impl_->stats.drain_calls;
    const auto deadline = std::chrono::steady_clock::now() + kDrainLimit;
    const bool physically_drained = impl_->pump_until_completion(
        [&] {
            for (const auto* slot : impl_->slots) {
                if (slot->submission.accepted &&
                    (slot->submission.queue.load(std::memory_order_acquire) == 0 ||
                     slot->submission.scope.load(std::memory_order_acquire) == 0)) {
                    return false;
                }
                if (slot->retired &&
                    ((slot->input_disposal_expected &&
                      !slot->input_disposal.observed.load(std::memory_order_acquire)) ||
                     (slot->output_disposal_expected &&
                      !slot->output_disposal.observed.load(std::memory_order_acquire)))) {
                    return false;
                }
            }
            return true;
        },
        deadline);
    for (auto* slot : impl_->slots)
        impl_->refresh(*slot, physically_drained);
    if (!physically_drained) {
        ++impl_->stats.failed_drains;
        // A deadline is not retirement evidence. Destroying the device asks
        // Dawn to close finite in-flight work, but ownership remains pinned
        // until a later drain observes every callback and disposal.
        impl_->reusable = false;
        if (!impl_->device_destroyed) {
            impl_->device.Destroy();
            impl_->device_destroyed = true;
            impl_->instance.ProcessEvents();
        }
        return false;
    }

    const bool terminals_published = impl_->pump_until_completion(
        [&] {
            bool complete = true;
            for (auto* slot : impl_->slots) {
                impl_->refresh(*slot, true);
                complete = complete && !slot->submission.accepted;
            }
            return complete;
        },
        deadline);
    if (!terminals_published) {
        ++impl_->stats.failed_drains;
        impl_->reusable = false;
        if (!impl_->device_destroyed) {
            impl_->device.Destroy();
            impl_->device_destroyed = true;
            impl_->instance.ProcessEvents();
        }
        return false;
    }

    bool poisoned = impl_->device_lost.load(std::memory_order_acquire) ||
                    impl_->uncaptured_error_generation.load(std::memory_order_acquire) != 0;
    for (const auto* slot : impl_->slots) {
        if (!slot->submission.accepted)
            continue;
        poisoned = poisoned || slot->submission.queue.load(std::memory_order_acquire) != 1 ||
                   slot->submission.scope.load(std::memory_order_acquire) != 1;
    }
    if (poisoned) {
        impl_->reusable = false;
        if (!impl_->device_destroyed) {
            impl_->device.Destroy();
            impl_->device_destroyed = true;
            impl_->instance.ProcessEvents();
        }
    }
    if (impl_->slots.empty() && impl_->reusable && !impl_->device_destroyed)
        impl_->accepting = true;
    return physically_drained;
}

bool DawnSharedIoProvider::device_lost() const noexcept {
    return impl_ &&
           (impl_->device_destroyed || impl_->device_lost.load(std::memory_order_acquire) ||
            dawn::native::IsDeviceLost(impl_->device.Get()));
}

std::uint32_t DawnSharedIoProvider::alignment() const noexcept {
    return impl_ ? impl_->alignment : 0;
}

std::uint64_t DawnSharedIoProvider::proc_table_install_count() const noexcept {
    auto& bootstrap = proc_bootstrap();
    std::lock_guard lock(bootstrap.mutex);
    return bootstrap.installs;
}

DawnSharedIoProvider::Stats DawnSharedIoProvider::stats() const noexcept {
    return impl_ ? impl_->stats : Stats{};
}

DawnSharedIoProvider::CompletionPolicy DawnSharedIoProvider::completion_policy() const noexcept {
    return impl_ ? impl_->completion_policy : CompletionPolicy::ProcessEvents;
}

DawnSharedIoProvider::AdapterIdentity DawnSharedIoProvider::adapter_identity() const {
    return impl_ ? impl_->adapter_identity : AdapterIdentity{};
}

} // namespace pulp::gpu_audio::detail
