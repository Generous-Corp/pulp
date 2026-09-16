#include "dawn_shared_io_provider.hpp"

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
        std::atomic<int> queue{0};
        std::atomic<int> scope{0};
        std::atomic<unsigned> scopes_pending{0};
        std::atomic<bool> scope_error{false};
        DawnSubmissionTracker tracker;
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
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

    explicit Impl(Options value) : options(std::move(value)) {}

    bool pump_until(const auto& done, std::chrono::steady_clock::time_point deadline) noexcept {
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            instance.ProcessEvents();
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
        }
        const auto scope_value = submission.scope.load(std::memory_order_acquire);
        if (!submission.scope_consumed && scope_value != 0) {
            submission.tracker.record_scope(submission.generation,
                                            scope_value == 1
                                                ? DawnSubmissionTracker::ScopeResult::Clean
                                                : DawnSubmissionTracker::ScopeResult::Error);
            submission.scope_consumed = true;
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
    bool accepting = false;
    bool reusable = true;
    bool device_destroyed = false;
    bool refusal_consumed = false;
    Stats stats;
    AdapterIdentity adapter_identity;
    std::shared_ptr<const void> lifetime = std::make_shared<int>(0);
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

bool DawnSharedIoProvider::submit(const SlotResources& resources, SlotToken token,
                                  std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept {
    auto* slot = static_cast<Impl::Slot*>(resources.opaque);
    if (!impl_->accepting || !slot || slot->retired || slot->submission.accepted ||
        (impl_->options.fault == Fault::RejectBeforeSubmit &&
         impl_->options.fault_slot == slot->index)) {
        return false;
    }
    auto& submission = slot->submission;
    ++submission.generation;
    if (!submission.tracker.begin(submission.generation, impl_->uncaptured_error_generation.load(
                                                             std::memory_order_acquire))) {
        return false;
    }
    submission.queue.store(0, std::memory_order_release);
    submission.scope.store(0, std::memory_order_release);
    submission.scopes_pending.store(3, std::memory_order_release);
    submission.scope_error.store(false, std::memory_order_release);
    submission.queue_consumed = false;
    submission.scope_consumed = false;
    submission.token = token;
    submission.inbox = std::move(terminal_inbox);
    submission.pending_terminal.reset();
    submission.forced_queue_result = impl_->options.fault == Fault::SyntheticQueueError       ? 3
                                     : impl_->options.fault == Fault::SyntheticQueueCancelled ? 2
                                                                                              : 0;
    submission.held_busy_claim.reset();
    submission.held_busy_released = false;

    impl_->push_error_scopes();
    if (impl_->options.fault == Fault::ForceLossBeforeSubmit)
        impl_->device.ForceLoss(wgpu::DeviceLostReason::Unknown, "P1 loss before submit");
    if (impl_->options.fault == Fault::PlantWriteBuffer) {
        constexpr float planted = -123.0f;
        impl_->queue.WriteBuffer(slot->input_buffer, 0, &planted, sizeof(planted));
    }
    auto encoder = impl_->device.CreateCommandEncoder();
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
    auto pass = encoder.BeginComputePass();
    pass.SetPipeline(impl_->pipeline);
    pass.SetBindGroup(0, slot->bind_group);
    const auto samples =
        std::min(slot->input_logical_bytes, slot->output_logical_bytes) / sizeof(float);
    pass.DispatchWorkgroups(static_cast<std::uint32_t>((samples + 63) / 64));
    if (impl_->options.fault == Fault::InvalidCommandAfterSubmit)
        pass.DispatchWorkgroups(std::numeric_limits<std::uint32_t>::max());
    pass.End();
    auto commands = encoder.Finish();
    impl_->queue.Submit(1, &commands);
    submission.accepted = true;
    if (impl_->options.fault == Fault::ForceLossBetweenSubmitAndCompletionRegistration)
        impl_->device.ForceLoss(wgpu::DeviceLostReason::Unknown,
                                "P1 loss before completion registration");
    impl_->queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowProcessEvents,
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
        impl_->device.PopErrorScope(
            wgpu::CallbackMode::AllowProcessEvents,
            [](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView,
               Impl::Submission* state) {
                const bool clean = status == wgpu::PopErrorScopeStatus::Success &&
                                   type == wgpu::ErrorType::NoError;
                if (!clean)
                    state->scope_error.store(true, std::memory_order_release);
                if (state->scopes_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    state->scope.store(state->scope_error.load(std::memory_order_acquire) ? 2 : 1,
                                       std::memory_order_release);
                }
            },
            &submission);
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
    impl_->instance.ProcessEvents();
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
    const bool physically_drained = impl_->pump_until(
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

    const bool terminals_published = impl_->pump_until(
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

DawnSharedIoProvider::AdapterIdentity DawnSharedIoProvider::adapter_identity() const {
    return impl_ ? impl_->adapter_identity : AdapterIdentity{};
}

} // namespace pulp::gpu_audio::detail
