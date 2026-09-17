#include "dawn/dawn_proc.h"
#include "dawn/dawn_version.h"
#include "dawn/native/DawnNative.h"
#include "support/dawn_transfer_call_counter.hpp"
#include "webgpu/webgpu_cpp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <sys/sysctl.h>
#include <sys/utsname.h>

#ifndef PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256
#define PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256 "unknown"
#endif

#ifndef PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256
#define PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256 "unknown"
#endif

#ifndef PULP_GPU_AUDIO_BUILD_TYPE
#define PULP_GPU_AUDIO_BUILD_TYPE "unknown"
#endif

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA "unknown"
#endif

namespace {

constexpr int kPassed = 0;
constexpr int kFailed = 1;
constexpr int kUnavailable = 77;
constexpr auto kWaitLimit = std::chrono::seconds(10);
constexpr auto kProbeLimit = std::chrono::seconds(15);

struct OwnedAllocation {
    std::atomic<void*> pointer{nullptr};
    std::atomic<unsigned> dispose_count{0};

    ~OwnedAllocation() {
        if (void* owned = pointer.exchange(nullptr, std::memory_order_acq_rel))
            std::free(owned);
    }
};

struct AdapterRequestState {
    std::atomic<bool> done{false};
    wgpu::RequestAdapterStatus status = wgpu::RequestAdapterStatus::Error;
    wgpu::Adapter adapter;
};

struct DeviceRequestState {
    std::atomic<bool> done{false};
    wgpu::RequestDeviceStatus status = wgpu::RequestDeviceStatus::Error;
    wgpu::Device device;
};

struct QueueCompletionState {
    std::atomic<bool> done{false};
    wgpu::QueueWorkDoneStatus status = wgpu::QueueWorkDoneStatus::Error;
};

struct ErrorScopeState {
    std::atomic<bool> done{false};
    wgpu::PopErrorScopeStatus status = wgpu::PopErrorScopeStatus::Error;
    wgpu::ErrorType type = wgpu::ErrorType::Unknown;
};

struct MapRequestState {
    std::atomic<bool> done{false};
    wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error;
};

enum class TransferNegativeControl {
    None,
    WriteBuffer,
    CopyBufferToBuffer,
    MapAsync,
};

const char* transfer_negative_control_name(TransferNegativeControl control) {
    switch (control) {
    case TransferNegativeControl::WriteBuffer:
        return "queue_write_buffer";
    case TransferNegativeControl::CopyBufferToBuffer:
        return "copy_buffer_to_buffer";
    case TransferNegativeControl::MapAsync:
        return "buffer_map_async";
    case TransferNegativeControl::None:
        return "none";
    }
    return "unknown";
}

void dispose_allocation(void* userdata) {
    auto* allocation = static_cast<OwnedAllocation*>(userdata);
    if (void* owned = allocation->pointer.exchange(nullptr, std::memory_order_acq_rel))
        std::free(owned);
    // Publish terminal disposal only after the allocation is no longer owned
    // by either the callback or the enclosing probe. The waiter uses an
    // acquire load before allowing stack teardown.
    allocation->dispose_count.fetch_add(1, std::memory_order_release);
}

std::string dawn_sha(const uint8_t* bytes) {
    if (bytes == nullptr)
        return "unknown";
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t index = 0; index < dawn::kDawnVersion.size(); ++index)
        out << std::setw(2) << unsigned(bytes[index]);
    return out.str();
}

std::string header_dawn_sha() {
    return dawn_sha(dawn::kDawnVersion.data());
}

std::string json_escape(wgpu::StringView value) {
    std::string out;
    for (size_t i = 0; i < value.length; ++i) {
        const char c = value.data[i];
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    }
    return out;
}

std::string machine_model() {
    size_t size = 0;
    if (sysctlbyname("hw.model", nullptr, &size, nullptr, 0) != 0 || size == 0)
        return "unknown";
    std::string model(size, '\0');
    if (sysctlbyname("hw.model", model.data(), &size, nullptr, 0) != 0)
        return "unknown";
    while (!model.empty() && model.back() == '\0')
        model.pop_back();
    return model;
}

std::string os_identity() {
    struct utsname info{};
    if (uname(&info) != 0)
        return "unknown";
    return std::string(info.sysname) + " " + info.release + " " + info.machine;
}

const char* backend_name(wgpu::BackendType backend) {
    switch (backend) {
    case wgpu::BackendType::Metal:
        return "metal";
    case wgpu::BackendType::Vulkan:
        return "vulkan";
    case wgpu::BackendType::D3D11:
        return "d3d11";
    case wgpu::BackendType::D3D12:
        return "d3d12";
    case wgpu::BackendType::OpenGL:
        return "opengl";
    case wgpu::BackendType::OpenGLES:
        return "opengles";
    case wgpu::BackendType::Null:
        return "null";
    default:
        return "undefined";
    }
}

const char* adapter_type_name(wgpu::AdapterType type) {
    switch (type) {
    case wgpu::AdapterType::IntegratedGPU:
        return "integrated-gpu";
    case wgpu::AdapterType::DiscreteGPU:
        return "discrete-gpu";
    case wgpu::AdapterType::CPU:
        return "cpu";
    default:
        return "unknown";
    }
}

struct Receipt {
    std::string status = "failed";
    std::string reason;
    std::string adapter;
    std::string machine;
    std::string os;
    std::string vendor;
    uint32_t vendor_id = 0;
    std::string adapter_type = "unknown";
    std::string backend = "undefined";
    uint32_t alignment = 0;
    bool feature = false;
    bool unsafe_toggle = false;
    bool oracle = false;
    unsigned validation_errors = 0;
    unsigned dispatches = 0;
    unsigned input_disposals = 0;
    unsigned output_disposals = 0;
    std::string expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA;
    std::string header_dawn_revision = header_dawn_sha();
    std::string proc_dawn_revision = "unknown";
    std::string native_dawn_revision = "unknown";
    std::string runtime_version_status = "unverified";
    std::string provider_identity_status = "unverified";
    bool proc_table_install_attempted = false;
    bool transfer_counter_measured = false;
    bool transfer_counter_oracle = false;
    std::string transfer_counter_negative_control = "none";
    std::uint64_t queue_write_buffer_calls = 0;
    std::uint64_t queue_write_buffer_bytes = 0;
    std::uint64_t copy_buffer_to_buffer_calls = 0;
    std::uint64_t copy_buffer_to_buffer_bytes = 0;
    std::uint64_t buffer_map_async_calls = 0;
    std::uint64_t buffer_map_async_bytes = 0;
    std::uint64_t queue_submit_calls = 0;
    std::uint64_t submitted_command_buffers = 0;
};

void print_receipt(const Receipt& r) {
    std::cout << "{\"schema\":\"pulp.gpu-host-mapped-pointer-probe.v2\""
              << ",\"status\":\"" << r.status << "\""
              << ",\"reason\":\"" << r.reason << "\""
              << ",\"dawn_sha\":\"" << r.header_dawn_revision << "\""
              << ",\"expected_dawn_revision\":\"" << r.expected_dawn_revision << "\""
              << ",\"header_dawn_revision\":\"" << r.header_dawn_revision << "\""
              << ",\"proc_dawn_revision\":\"" << r.proc_dawn_revision << "\""
              << ",\"native_dawn_revision\":\"" << r.native_dawn_revision << "\""
              << ",\"runtime_version_status\":\"" << r.runtime_version_status << "\""
              << ",\"provider_identity_status\":\"" << r.provider_identity_status << "\""
              << ",\"proc_table_install_attempted\":"
              << (r.proc_table_install_attempted ? "true" : "false")
              << ",\"provider_asset_sha256\":\"" << PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256 << "\""
              << ",\"dawn_archive_sha256\":\"" << PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256 << "\""
              << ",\"build_type\":\"" << PULP_GPU_AUDIO_BUILD_TYPE << "\""
              << ",\"os\":\"" << r.os << "\""
              << ",\"machine\":\"" << r.machine << "\""
              << ",\"adapter\":\"" << r.adapter << "\""
              << ",\"vendor\":\"" << r.vendor << "\""
              << ",\"vendor_id\":" << r.vendor_id << ",\"adapter_type\":\"" << r.adapter_type
              << "\""
              << ",\"backend\":\"" << r.backend << "\""
              << ",\"alignment\":" << r.alignment
              << ",\"host_mapped_pointer\":" << (r.feature ? "true" : "false")
              << ",\"allow_unsafe_apis\":" << (r.unsafe_toggle ? "true" : "false")
              << ",\"oracle\":" << (r.oracle ? "true" : "false")
              << ",\"validation_errors\":" << r.validation_errors
              << ",\"dispatches\":" << r.dispatches << ",\"input_disposals\":" << r.input_disposals
              << ",\"output_disposals\":" << r.output_disposals << ",\"transfer_counter_measured\":"
              << (r.transfer_counter_measured ? "true" : "false")
              << ",\"transfer_counter_oracle\":" << (r.transfer_counter_oracle ? "true" : "false")
              << ",\"transfer_counter_negative_control\":\"" << r.transfer_counter_negative_control
              << "\""
              << ",\"queue_write_buffer_calls\":" << r.queue_write_buffer_calls
              << ",\"queue_write_buffer_bytes\":" << r.queue_write_buffer_bytes
              << ",\"copy_buffer_to_buffer_calls\":" << r.copy_buffer_to_buffer_calls
              << ",\"copy_buffer_to_buffer_bytes\":" << r.copy_buffer_to_buffer_bytes
              << ",\"buffer_map_async_calls\":" << r.buffer_map_async_calls
              << ",\"buffer_map_async_bytes\":" << r.buffer_map_async_bytes
              << ",\"queue_submit_calls\":" << r.queue_submit_calls
              << ",\"submitted_command_buffers\":" << r.submitted_command_buffers << "}\n";
}

template <typename Predicate>
bool pump_until(wgpu::Instance& instance, std::chrono::steady_clock::time_point probe_deadline,
                Predicate done) {
    const auto deadline = std::min(probe_deadline, std::chrono::steady_clock::now() + kWaitLimit);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        instance.ProcessEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

size_t round_up(size_t value, size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

bool allocate(OwnedAllocation& allocation, size_t alignment, size_t size) {
    void* pointer = nullptr;
    if (posix_memalign(&pointer, alignment, size) != 0)
        return false;
    allocation.pointer.store(pointer, std::memory_order_release);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Receipt receipt;
    // These outlive every Dawn object so a late teardown callback can never
    // dereference dead stack storage after a timeout or validation failure.
    OwnedAllocation input;
    OwnedAllocation output;
    auto validation_errors = std::make_shared<std::atomic<unsigned>>(0);
    auto finish = [&](int code, std::string status, std::string reason) {
        receipt.status = std::move(status);
        receipt.reason = std::move(reason);
        print_receipt(receipt);
        return code;
    };
    auto finish_feature_gate = [&](int code, std::string status, std::string reason) {
        receipt.validation_errors = validation_errors->load();
        if (code == kPassed && receipt.validation_errors != 0) {
            code = kFailed;
            status = "failed";
            reason = "feature_gate_validation_error";
        }
        return finish(code, std::move(status), std::move(reason));
    };
    bool verify_oracle_negative_control = false;
    bool verify_feature_gate_negative_control = false;
    bool verify_setter_order_negative_control = false;
    const char* provider_identity_negative_control = nullptr;
    TransferNegativeControl transfer_negative_control = TransferNegativeControl::None;
    if (argc == 2 && std::strcmp(argv[1], "--verify-oracle-negative-control") == 0) {
        verify_oracle_negative_control = true;
    } else if (argc >= 2 && argc <= 3 &&
               std::strcmp(argv[1], "--verify-provider-identity-negative-control") == 0) {
        provider_identity_negative_control = argc == 3 ? argv[2] : "proc";
        if (std::strcmp(provider_identity_negative_control, "expected") != 0 &&
            std::strcmp(provider_identity_negative_control, "header") != 0 &&
            std::strcmp(provider_identity_negative_control, "proc") != 0 &&
            std::strcmp(provider_identity_negative_control, "native") != 0)
            return finish(kFailed, "failed", "invalid_arguments");
    } else if (argc == 2 &&
               std::strcmp(argv[1], "--verify-provider-setter-order-negative-control") == 0) {
        provider_identity_negative_control = "proc";
        verify_setter_order_negative_control = true;
    } else if (argc == 2 && std::strcmp(argv[1], "--verify-feature-gate-negative-control") == 0) {
        verify_feature_gate_negative_control = true;
    } else if (argc == 2 &&
               std::strcmp(argv[1], "--verify-write-buffer-counter-negative-control") == 0) {
        transfer_negative_control = TransferNegativeControl::WriteBuffer;
    } else if (argc == 2 &&
               std::strcmp(argv[1], "--verify-copy-buffer-counter-negative-control") == 0) {
        transfer_negative_control = TransferNegativeControl::CopyBufferToBuffer;
    } else if (argc == 2 &&
               std::strcmp(argv[1], "--verify-map-async-counter-negative-control") == 0) {
        transfer_negative_control = TransferNegativeControl::MapAsync;
    } else if (argc != 1) {
        return finish(kFailed, "failed", "invalid_arguments");
    }
    receipt.transfer_counter_negative_control =
        transfer_negative_control_name(transfer_negative_control);

#if !defined(__aarch64__) && !defined(__arm64__)
    return finish(kUnavailable, "unavailable", "apple_silicon_required");
#endif
    if (std::strcmp(PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256, "unknown") == 0)
        return finish(kFailed, "failed", "provider_identity_unknown");
    receipt.machine = machine_model();
    receipt.os = os_identity();

    // Validate the compile-time header, libdawn_proc entry point, and native
    // proc table before installing that table process-wide. Calling the setter
    // first would let a mixed provider execute through an ABI it has not proved.
    receipt.proc_dawn_revision = dawn_sha(dawnProcGetVersion());
    const DawnProcTable& procs = dawn::native::GetProcs();
    receipt.native_dawn_revision = dawn_sha(procs.version);
    auto install_proc_table = [&] {
        receipt.proc_table_install_attempted = true;
        dawnProcSetProcs(&procs);
    };
    if (verify_setter_order_negative_control)
        install_proc_table();
    auto corrupt_revision = [](std::string& revision) {
        if (revision.size() != dawn::kDawnVersion.size() * 2)
            revision.assign(dawn::kDawnVersion.size() * 2, '0');
        revision.front() = revision.front() == '0' ? '1' : '0';
    };
    if (provider_identity_negative_control != nullptr) {
        if (std::strcmp(provider_identity_negative_control, "expected") == 0)
            corrupt_revision(receipt.expected_dawn_revision);
        else if (std::strcmp(provider_identity_negative_control, "header") == 0)
            corrupt_revision(receipt.header_dawn_revision);
        else if (std::strcmp(provider_identity_negative_control, "proc") == 0)
            corrupt_revision(receipt.proc_dawn_revision);
        else
            corrupt_revision(receipt.native_dawn_revision);
    }
    const bool has_exact_expectation = receipt.expected_dawn_revision != "unknown";
    const bool expected_matches =
        !has_exact_expectation || receipt.header_dawn_revision == receipt.expected_dawn_revision;
    const bool runtime_versions_match =
        receipt.header_dawn_revision == receipt.proc_dawn_revision &&
        receipt.header_dawn_revision == receipt.native_dawn_revision;
    receipt.runtime_version_status = runtime_versions_match ? "passed" : "failed";
    if (!expected_matches || !runtime_versions_match) {
        receipt.provider_identity_status = "failed";
        return finish(kFailed, "failed", "provider_identity_mismatch");
    }
    receipt.provider_identity_status = has_exact_expectation ? "passed" : "unverified";
    install_proc_table();
    // This must precede every Dawn object below. Reverse destruction then
    // retires all wrappers and callbacks before the process-global proc table
    // is restored by the counter.
    pulp::test::DawnTransferCallCounter transfer_counter;

    wgpu::InstanceDescriptor instance_desc{};
    auto native_instance = std::make_unique<dawn::native::Instance>(
        reinterpret_cast<const WGPUInstanceDescriptor*>(&instance_desc));
    wgpu::Instance instance(native_instance->Get());
    if (!instance)
        return finish(kFailed, "failed", "instance_creation_failed");
    const auto probe_deadline = std::chrono::steady_clock::now() + kProbeLimit;

    const char* enabled_toggles[] = {"allow_unsafe_apis"};
    wgpu::DawnTogglesDescriptor adapter_toggles{};
    adapter_toggles.enabledToggleCount = 1;
    adapter_toggles.enabledToggles = enabled_toggles;
    wgpu::RequestAdapterOptions adapter_options{};
    adapter_options.powerPreference = wgpu::PowerPreference::HighPerformance;
    adapter_options.nextInChain = &adapter_toggles;

    auto adapter_request = std::make_shared<AdapterRequestState>();
    instance.RequestAdapter(&adapter_options, wgpu::CallbackMode::AllowProcessEvents,
                            [adapter_request](wgpu::RequestAdapterStatus status,
                                              wgpu::Adapter result, wgpu::StringView) {
                                adapter_request->status = status;
                                if (status == wgpu::RequestAdapterStatus::Success)
                                    adapter_request->adapter = std::move(result);
                                adapter_request->done = true;
                            });
    if (!pump_until(instance, probe_deadline, [&] { return adapter_request->done.load(); })) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kFailed, "failed", "adapter_request_timeout");
        return finish(kFailed, "failed", "adapter_request_timeout");
    }
    if (adapter_request->status != wgpu::RequestAdapterStatus::Success ||
        !adapter_request->adapter) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kUnavailable, "unavailable", "no_adapter");
        return finish(kUnavailable, "unavailable", "no_adapter");
    }
    wgpu::Adapter adapter = std::move(adapter_request->adapter);

    wgpu::AdapterInfo info{};
    if (adapter.GetInfo(&info) != wgpu::Status::Success) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kFailed, "failed", "adapter_info_failed");
        return finish(kFailed, "failed", "adapter_info_failed");
    }
    receipt.adapter = json_escape(info.device);
    receipt.vendor = json_escape(info.vendor);
    receipt.vendor_id = info.vendorID;
    receipt.adapter_type = adapter_type_name(info.adapterType);
    receipt.backend = backend_name(info.backendType);
    if (info.backendType != wgpu::BackendType::Metal) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kUnavailable, "unavailable", "metal_adapter_required");
        return finish(kUnavailable, "unavailable", "metal_adapter_required");
    }
    if (receipt.vendor_id != 0x106b) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kUnavailable, "unavailable", "apple_gpu_required");
        return finish(kUnavailable, "unavailable", "apple_gpu_required");
    }

    receipt.feature = adapter.HasFeature(wgpu::FeatureName::HostMappedPointer);
    if (!receipt.feature) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kUnavailable, "unavailable",
                                       "enabled_positive_control_feature_missing");
        return finish(kUnavailable, "unavailable", "host_mapped_pointer_missing");
    }

    wgpu::DawnHostMappedPointerLimits host_limits{};
    wgpu::Limits limits{};
    limits.nextInChain = &host_limits;
    if (adapter.GetLimits(&limits) != wgpu::Status::Success ||
        host_limits.hostMappedPointerAlignment == wgpu::kLimitU32Undefined ||
        host_limits.hostMappedPointerAlignment == 0) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kFailed, "failed", "host_pointer_alignment_unavailable");
        return finish(kFailed, "failed", "host_pointer_alignment_unavailable");
    }
    receipt.alignment = host_limits.hostMappedPointerAlignment;

    wgpu::DawnTogglesDescriptor device_toggles{};
    device_toggles.enabledToggleCount = 1;
    device_toggles.enabledToggles = enabled_toggles;
    wgpu::FeatureName required_features[] = {wgpu::FeatureName::HostMappedPointer};
    wgpu::DeviceDescriptor device_desc{};
    device_desc.label = "Pulp GPU audio shared-memory P0 probe";
    device_desc.nextInChain = &device_toggles;
    device_desc.requiredFeatureCount = 1;
    device_desc.requiredFeatures = required_features;
    device_desc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType,
                                              wgpu::StringView,
                                              std::atomic<unsigned>* errors) { ++*errors; },
                                           validation_errors.get());
    receipt.unsafe_toggle = true;

    auto device_request = std::make_shared<DeviceRequestState>();
    adapter.RequestDevice(
        &device_desc, wgpu::CallbackMode::AllowProcessEvents,
        [device_request](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView) {
            device_request->status = status;
            if (status == wgpu::RequestDeviceStatus::Success)
                device_request->device = std::move(result);
            device_request->done = true;
        });
    if (!pump_until(instance, probe_deadline, [&] { return device_request->done.load(); })) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kFailed, "failed",
                                       "enabled_positive_control_device_request_timeout");
        return finish(kFailed, "failed", "device_request_timeout");
    }
    if (device_request->status != wgpu::RequestDeviceStatus::Success) {
        if (verify_feature_gate_negative_control) {
            const char* reason =
                device_request->status == wgpu::RequestDeviceStatus::CallbackCancelled
                    ? "enabled_positive_control_device_request_cancelled"
                    : "enabled_positive_control_device_request_rejected";
            return finish_feature_gate(kFailed, "failed", reason);
        }
        return finish(kFailed, "failed", "device_creation_failed");
    }
    if (!device_request->device) {
        if (verify_feature_gate_negative_control)
            return finish_feature_gate(kFailed, "failed",
                                       "enabled_positive_control_null_device_on_success");
        return finish(kFailed, "failed", "device_creation_failed");
    }
    if (verify_feature_gate_negative_control) {
        // The adapter was discovered with unsafe APIs enabled so that it advertises
        // HostMappedPointer. Keep that adapter and required feature identical to the
        // successful request above, whose device remains alive in device_request,
        // then prove device creation rejects the feature when its unsafe toggle is
        // explicitly disabled.
        wgpu::DawnTogglesDescriptor disabled_device_toggles{};
        disabled_device_toggles.disabledToggleCount = 1;
        disabled_device_toggles.disabledToggles = enabled_toggles;
        wgpu::DeviceDescriptor disabled_device_desc{};
        disabled_device_desc.label = "Pulp GPU audio feature-gate negative control";
        disabled_device_desc.nextInChain = &disabled_device_toggles;
        disabled_device_desc.requiredFeatureCount = 1;
        disabled_device_desc.requiredFeatures = required_features;
        disabled_device_desc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView,
               std::atomic<unsigned>* errors) { ++*errors; },
            validation_errors.get());

        auto disabled_request = std::make_shared<DeviceRequestState>();
        adapter.RequestDevice(&disabled_device_desc, wgpu::CallbackMode::AllowProcessEvents,
                              [disabled_request](wgpu::RequestDeviceStatus status,
                                                 wgpu::Device result, wgpu::StringView) {
                                  disabled_request->status = status;
                                  if (status == wgpu::RequestDeviceStatus::Success)
                                      disabled_request->device = std::move(result);
                                  disabled_request->done = true;
                              });
        if (!pump_until(instance, probe_deadline, [&] { return disabled_request->done.load(); }))
            return finish_feature_gate(kFailed, "failed", "feature_gate_device_request_timeout");
        if (disabled_request->status == wgpu::RequestDeviceStatus::Success) {
            const char* reason = disabled_request->device ? "feature_gate_device_request_accepted"
                                                          : "feature_gate_null_device_on_success";
            return finish_feature_gate(kFailed, "failed", reason);
        }
        if (disabled_request->status == wgpu::RequestDeviceStatus::CallbackCancelled)
            return finish_feature_gate(kFailed, "failed", "feature_gate_device_request_cancelled");
        if (disabled_request->status != wgpu::RequestDeviceStatus::Error)
            return finish_feature_gate(kFailed, "failed", "feature_gate_request_unexpected_status");
        // RequestDevice error strings are diagnostic text, not a stable API contract. The
        // causal control is the adjacent successful request on this same adapter with the
        // same required feature; this request differs only by explicitly disabling the
        // feature's unsafe-API toggle and must return Error without a device.
        return finish_feature_gate(
            kPassed, "passed",
            "required_feature_device_request_rejected_when_unsafe_toggle_disabled");
    }
    wgpu::Device device = std::move(device_request->device);
    auto pop_error_scope = [&](const std::shared_ptr<ErrorScopeState>& state) {
        device.PopErrorScope(
            wgpu::CallbackMode::AllowProcessEvents,
            [state](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
                state->status = status;
                state->type = type;
                state->done = true;
            });
        return pump_until(instance, probe_deadline, [&] { return state->done.load(); }) &&
               state->status == wgpu::PopErrorScopeStatus::Success &&
               state->type == wgpu::ErrorType::NoError;
    };

    const size_t allocation_alignment = std::max<size_t>(receipt.alignment, sizeof(void*));
    const size_t bytes = round_up(4096, allocation_alignment);
    const size_t samples = bytes / sizeof(float);
    if (!allocate(input, allocation_alignment, bytes) ||
        !allocate(output, allocation_alignment, bytes)) {
        return finish(kFailed, "failed", "aligned_allocation_failed");
    }
    auto* input_samples = static_cast<float*>(input.pointer.load(std::memory_order_acquire));
    auto* output_samples = static_cast<float*>(output.pointer.load(std::memory_order_acquire));
    for (size_t i = 0; i < samples; ++i) {
        input_samples[i] = static_cast<float>(i % 97) * 0.125f - 3.0f;
        output_samples[i] = -9999.0f;
    }

    wgpu::BufferHostMappedPointer input_host{};
    input_host.pointer = input_samples;
    input_host.disposeCallback = dispose_allocation;
    input_host.userdata = &input;
    wgpu::BufferDescriptor input_desc{};
    input_desc.label = "P0 shared input";
    input_desc.nextInChain = &input_host;
    input_desc.size = bytes;
    input_desc.usage = wgpu::BufferUsage::Storage;

    wgpu::BufferHostMappedPointer output_host{};
    output_host.pointer = output_samples;
    output_host.disposeCallback = dispose_allocation;
    output_host.userdata = &output;
    wgpu::BufferDescriptor output_desc{};
    output_desc.label = "P0 shared output";
    output_desc.nextInChain = &output_host;
    output_desc.size = bytes;
    output_desc.usage = wgpu::BufferUsage::Storage;

    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::Buffer input_buffer = device.CreateBuffer(&input_desc);
    wgpu::Buffer output_buffer = device.CreateBuffer(&output_desc);
    auto buffer_scope = std::make_shared<ErrorScopeState>();
    const bool buffers_valid = pop_error_scope(buffer_scope);
    if (!buffers_valid || !input_buffer || !output_buffer) {
        input_buffer = nullptr;
        output_buffer = nullptr;
        pump_until(instance, probe_deadline, [&] {
            return input.dispose_count.load(std::memory_order_acquire) == 1 &&
                   output.dispose_count.load(std::memory_order_acquire) == 1;
        });
        receipt.input_disposals = input.dispose_count;
        receipt.output_disposals = output.dispose_count;
        return finish(kFailed, "failed", "host_buffer_creation_failed");
    }
    auto release_host_buffers = [&] {
        input_buffer.Destroy();
        output_buffer.Destroy();
        input_buffer = nullptr;
        output_buffer = nullptr;
        const bool disposed = pump_until(instance, probe_deadline, [&] {
            return input.dispose_count.load(std::memory_order_acquire) == 1 &&
                   output.dispose_count.load(std::memory_order_acquire) == 1;
        });
        receipt.input_disposals = input.dispose_count;
        receipt.output_disposals = output.dispose_count;
        return disposed && receipt.input_disposals == 1 && receipt.output_disposals == 1;
    };

    constexpr const char* shader = R"wgsl(
@group(0) @binding(0) var<storage, read> input_data : array<f32>;
@group(0) @binding(1) var<storage, read_write> output_data : array<f32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    if (gid.x < arrayLength(&output_data)) {
        output_data[gid.x] = input_data[gid.x] * 1.75 + 0.25;
    }
}
)wgsl";
    wgpu::ShaderSourceWGSL source{};
    source.code = shader;
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.nextInChain = &source;
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    const auto module = device.CreateShaderModule(&shader_desc);
    wgpu::ComputePipelineDescriptor pipeline_desc{};
    pipeline_desc.compute.module = module;
    pipeline_desc.compute.entryPoint = "main";
    const auto pipeline = device.CreateComputePipeline(&pipeline_desc);
    auto pipeline_scope = std::make_shared<ErrorScopeState>();
    const bool pipeline_valid = pop_error_scope(pipeline_scope);
    if (!pipeline_valid || !module || !pipeline) {
        release_host_buffers();
        return finish(kFailed, "failed", "pipeline_creation_failed");
    }

    std::array<wgpu::BindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].buffer = input_buffer;
    entries[0].size = bytes;
    entries[1].binding = 1;
    entries[1].buffer = output_buffer;
    entries[1].size = bytes;
    wgpu::BindGroupDescriptor bind_group_desc{};
    bind_group_desc.layout = pipeline.GetBindGroupLayout(0);
    bind_group_desc.entryCount = entries.size();
    bind_group_desc.entries = entries.data();
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    const auto bind_group = device.CreateBindGroup(&bind_group_desc);
    auto bind_group_scope = std::make_shared<ErrorScopeState>();
    const bool bind_group_valid = pop_error_scope(bind_group_scope);
    if (!bind_group_valid || !bind_group) {
        release_host_buffers();
        return finish(kFailed, "failed", "bind_group_creation_failed");
    }

    auto queue = device.GetQueue();

    constexpr std::uint64_t kControlBytes = sizeof(std::uint32_t);
    wgpu::Buffer transfer_control_source;
    wgpu::Buffer transfer_control_destination;
    if (transfer_negative_control != TransferNegativeControl::None) {
        device.PushErrorScope(wgpu::ErrorFilter::Validation);
        wgpu::BufferDescriptor control_desc{};
        control_desc.size = kControlBytes;
        switch (transfer_negative_control) {
        case TransferNegativeControl::WriteBuffer:
            control_desc.label = "P1 WriteBuffer counter control";
            control_desc.usage = wgpu::BufferUsage::CopyDst;
            transfer_control_destination = device.CreateBuffer(&control_desc);
            break;
        case TransferNegativeControl::CopyBufferToBuffer:
            control_desc.label = "P1 copy counter source";
            control_desc.usage = wgpu::BufferUsage::CopySrc;
            transfer_control_source = device.CreateBuffer(&control_desc);
            control_desc.label = "P1 copy counter destination";
            control_desc.usage = wgpu::BufferUsage::CopyDst;
            transfer_control_destination = device.CreateBuffer(&control_desc);
            break;
        case TransferNegativeControl::MapAsync:
            control_desc.label = "P1 MapAsync counter control";
            control_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
            transfer_control_destination = device.CreateBuffer(&control_desc);
            break;
        case TransferNegativeControl::None:
            break;
        }
        auto control_scope = std::make_shared<ErrorScopeState>();
        const bool controls_valid = pop_error_scope(control_scope);
        const bool source_required =
            transfer_negative_control == TransferNegativeControl::CopyBufferToBuffer;
        if (!controls_valid || !transfer_control_destination ||
            (source_required && !transfer_control_source)) {
            release_host_buffers();
            return finish(kFailed, "failed", "transfer_counter_control_buffer_creation_failed");
        }
    }

    // Setup is complete and the queue is idle. Measure only the two-dispatch
    // data-plane interval (plus the one requested planted operation).
    transfer_counter.reset();
    if (transfer_negative_control == TransferNegativeControl::WriteBuffer) {
        constexpr std::uint32_t planted_value = 0x504c5055;
        queue.WriteBuffer(transfer_control_destination, 0, &planted_value, sizeof(planted_value));
    } else if (transfer_negative_control == TransferNegativeControl::MapAsync) {
        auto map_request = std::make_shared<MapRequestState>();
        transfer_control_destination.MapAsync(
            wgpu::MapMode::Read, 0, kControlBytes, wgpu::CallbackMode::AllowProcessEvents,
            [map_request](wgpu::MapAsyncStatus status, wgpu::StringView) {
                map_request->status = status;
                map_request->done = true;
            });
        if (!pump_until(instance, probe_deadline, [&] { return map_request->done.load(); })) {
            release_host_buffers();
            return finish(kFailed, "failed", "transfer_counter_map_timeout");
        }
        if (map_request->status != wgpu::MapAsyncStatus::Success) {
            release_host_buffers();
            return finish(kFailed, "failed", "transfer_counter_map_failed");
        }
        transfer_control_destination.Unmap();
    }

    auto dispatch = [&](bool plant_copy) {
        auto encoder = device.CreateCommandEncoder();
        if (plant_copy) {
            encoder.CopyBufferToBuffer(transfer_control_source, 0, transfer_control_destination, 0,
                                       kControlBytes);
        }
        auto pass = encoder.BeginComputePass();
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bind_group);
        pass.DispatchWorkgroups(static_cast<uint32_t>((samples + 63) / 64));
        pass.End();
        auto commands = encoder.Finish();
        queue.Submit(1, &commands);

        auto completion = std::make_shared<QueueCompletionState>();
        queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowProcessEvents,
                                  [completion](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
                                      completion->status = status;
                                      completion->done = true;
                                  });
        const bool completed =
            pump_until(instance, probe_deadline, [&] { return completion->done.load(); }) &&
            completion->status == wgpu::QueueWorkDoneStatus::Success;
        if (completed)
            ++receipt.dispatches;
        return completed;
    };
    auto output_matches = [&] {
        for (size_t i = 0; i < samples; ++i) {
            const float expected = input_samples[i] * 1.75f + 0.25f;
            const float actual = output_samples[i];
            if (!std::isfinite(actual) || !(std::abs(actual - expected) <= 1.0e-6f))
                return false;
        }
        return true;
    };

    if (!dispatch(transfer_negative_control == TransferNegativeControl::CopyBufferToBuffer) ||
        !output_matches()) {
        release_host_buffers();
        return finish(kFailed, "failed", "first_round_trip_failed");
    }

    // Reuse the same imports after CPU-side mutation. This distinguishes
    // persistent shared memory from a one-time import of initial contents.
    for (size_t i = 0; i < samples; ++i) {
        input_samples[i] = static_cast<float>((i * 7) % 113) * -0.0625f + 2.0f;
        output_samples[i] = 7777.0f;
    }
    if (!dispatch(false) || !output_matches()) {
        release_host_buffers();
        return finish(kFailed, "failed", "persistent_reuse_failed");
    }

    const auto transfer_snapshot = transfer_counter.snapshot();
    receipt.transfer_counter_measured = true;
    receipt.queue_write_buffer_calls = transfer_snapshot.queue_write_buffer_calls;
    receipt.queue_write_buffer_bytes = transfer_snapshot.queue_write_buffer_bytes;
    receipt.copy_buffer_to_buffer_calls = transfer_snapshot.copy_buffer_to_buffer_calls;
    receipt.copy_buffer_to_buffer_bytes = transfer_snapshot.copy_buffer_to_buffer_bytes;
    receipt.buffer_map_async_calls = transfer_snapshot.buffer_map_async_calls;
    receipt.buffer_map_async_bytes = transfer_snapshot.buffer_map_async_bytes;
    receipt.queue_submit_calls = transfer_snapshot.queue_submit_calls;
    receipt.submitted_command_buffers = transfer_snapshot.submitted_command_buffers;

    const bool submission_counts_match =
        receipt.queue_submit_calls == 2 && receipt.submitted_command_buffers == 2;
    const bool no_write =
        receipt.queue_write_buffer_calls == 0 && receipt.queue_write_buffer_bytes == 0;
    const bool no_copy =
        receipt.copy_buffer_to_buffer_calls == 0 && receipt.copy_buffer_to_buffer_bytes == 0;
    const bool no_map = receipt.buffer_map_async_calls == 0 && receipt.buffer_map_async_bytes == 0;
    switch (transfer_negative_control) {
    case TransferNegativeControl::None:
        receipt.transfer_counter_oracle = submission_counts_match && no_write && no_copy && no_map;
        break;
    case TransferNegativeControl::WriteBuffer:
        receipt.transfer_counter_oracle =
            submission_counts_match && receipt.queue_write_buffer_calls == 1 &&
            receipt.queue_write_buffer_bytes == kControlBytes && no_copy && no_map;
        break;
    case TransferNegativeControl::CopyBufferToBuffer:
        receipt.transfer_counter_oracle =
            submission_counts_match && no_write && receipt.copy_buffer_to_buffer_calls == 1 &&
            receipt.copy_buffer_to_buffer_bytes == kControlBytes && no_map;
        break;
    case TransferNegativeControl::MapAsync:
        receipt.transfer_counter_oracle = submission_counts_match && no_write && no_copy &&
                                          receipt.buffer_map_async_calls == 1 &&
                                          receipt.buffer_map_async_bytes == kControlBytes;
        break;
    }
    if (!receipt.transfer_counter_oracle) {
        release_host_buffers();
        return finish(kFailed, "failed", "transfer_counter_oracle_mismatch");
    }

    receipt.validation_errors = validation_errors->load();
    if (receipt.validation_errors != 0) {
        release_host_buffers();
        return finish(kFailed, "failed", "validation_error");
    }
    receipt.oracle = true;
    if (verify_oracle_negative_control) {
        output_samples[0] = std::numeric_limits<float>::quiet_NaN();
        receipt.oracle = output_matches();
    }

    // Destroy initiates retirement even while bind groups retain references;
    // the disposal callbacks below are the proof that reclamation is safe.
    const bool disposed = release_host_buffers();

    if (!disposed) {
        return finish(kFailed, "failed", "disposal_contract_failed");
    }
    receipt.validation_errors = validation_errors->load();
    if (receipt.validation_errors != 0)
        return finish(kFailed, "failed", "validation_error_after_disposal");
    if (verify_oracle_negative_control) {
        if (receipt.oracle) {
            return finish(kFailed, "failed", "oracle_negative_control_missed");
        }
        return finish(kPassed, "passed", "oracle_negative_control_detected");
    }
    if (transfer_negative_control != TransferNegativeControl::None)
        return finish(kPassed, "passed", "transfer_counter_negative_control_detected");
    if (!receipt.oracle)
        return finish(kFailed, "failed", "oracle_mismatch");
    return finish(kPassed, "passed", "ok");
}
