#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace pulp::host {

class PluginSlot;
struct PluginInfo;

/// Durable Timeline binding key for Pulp's pathless basic instrument.
inline constexpr std::string_view kBasicInstrumentBindingKey = "pulp.instrument.basic";

/// Durable Timeline binding key for Pulp's pathless look-ahead note humaniser.
inline constexpr std::string_view kEventHumaniserBindingKey = "pulp.device.event.humanise";

/// The humaniser's look-ahead window, in samples, and therefore the latency it
/// reports. A note can only be displaced by a device that received it before
/// its nominal time, so the window is what buys the device the right to place
/// a note anywhere in `[nominal - window, nominal]` once the host has shifted
/// the scheduling window by the same amount. It is a compile-time constant and
/// no parameter changes it: a latency that moved under automation would
/// invalidate the shift the host already resolved and cached.
inline constexpr int kEventHumaniserWindowSamples = 512;

/// The longest device chain the host binding lowers today. Any number of
/// event-to-event devices would be sound, but each one needs its own prepared
/// node and event edge, and two is what the graph, the shift accumulator, and
/// the reconciler are proven against. A longer chain is refused at admission
/// rather than truncated, and the bound is public so a caller can predict that
/// refusal instead of discovering it on a round trip.
inline constexpr std::size_t kAdmittedDeviceChainLength = 2;

/// The domain a built-in Timeline device occupies in a track's device chain.
/// Deliberately not `timeline::DeviceSlotKind`: the catalog is consumed by
/// tools that project it to CLI and MCP surfaces and must not drag the whole
/// Timeline document model along with it.
enum class BuiltInDeviceDomain { EventToEvent, EventToAudio };

/// One instantiable Pulp-owned device. Every field a caller needs to author a
/// placement that the host binding will actually admit, plus the latency the
/// device reports so a client can explain the compensation before playing.
struct BuiltInDeviceDescriptor {
    std::string_view binding_key;
    std::string_view display_name;
    std::string_view manufacturer;
    std::string_view category;
    std::string_view summary;
    BuiltInDeviceDomain domain = BuiltInDeviceDomain::EventToAudio;
    int num_audio_inputs = 0;
    int num_audio_outputs = 2;
    int latency_samples = 0;
    bool is_instrument = false;
};

/// Every device `load_builtin_plugin` can instantiate, in chain order: an
/// event-to-event device may precede an event-to-audio one, never follow it.
std::span<const BuiltInDeviceDescriptor> builtin_device_catalog() noexcept;

/// The descriptor for `binding_key`, or nullptr when no built-in owns it.
const BuiltInDeviceDescriptor* find_builtin_device(std::string_view binding_key) noexcept;

/// Factory signature used by Timeline's disposable graph-lowering transaction.
using TimelineDeviceSlotFactory = std::unique_ptr<PluginSlot> (*)(const PluginInfo&);

/// Loads a pathless Pulp-owned device. Unknown keys and non-empty paths fail closed.
std::unique_ptr<PluginSlot> load_builtin_plugin(const PluginInfo& info);

/// The per-chain event-compensation ceiling, in samples. A chain whose
/// accumulated device latency exceeds it is refused rather than compensated.
/// Exposed as a function so the value stays owned by the graph binding that
/// enforces it and no caller can drift from it by re-declaring a constant.
int event_device_latency_ceiling_samples() noexcept;

/// The `PluginInfo` the host uses to instantiate `descriptor` through the
/// device factory. Exposed so a caller can build the same descriptor the
/// binding would, without reaching into the resolver.
PluginInfo builtin_device_plugin_info(const BuiltInDeviceDescriptor& descriptor);

} // namespace pulp::host
