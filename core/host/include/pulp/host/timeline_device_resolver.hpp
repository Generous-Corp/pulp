#pragma once

#include <memory>
#include <string_view>

namespace pulp::host {

class PluginSlot;
struct PluginInfo;

/// Durable Timeline binding key for Pulp's pathless basic instrument.
inline constexpr std::string_view kBasicInstrumentBindingKey = "pulp.instrument.basic";

/// Factory signature used by Timeline's disposable graph-lowering transaction.
using TimelineDeviceSlotFactory = std::unique_ptr<PluginSlot> (*)(const PluginInfo&);

/// Loads a pathless Pulp-owned device. Unknown keys and non-empty paths fail closed.
std::unique_ptr<PluginSlot> load_builtin_plugin(const PluginInfo& info);

} // namespace pulp::host
