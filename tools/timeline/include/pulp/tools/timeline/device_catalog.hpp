#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::tools::timeline {

/** @addtogroup tools_timeline
 * @{
 */

struct DeviceParameterCatalogEntry {
    std::uint32_t id = 0;
    std::string name;
    std::string unit;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    bool automatable = false;
    bool read_only = false;
    bool hidden = false;
    bool stepped = false;
    bool is_bypass = false;
    bool rampable = false;
    bool modulatable = false;
};

/// One instantiable Pulp-owned Timeline device, projected for offline callers.
///
/// Carries its own value types rather than the host descriptor's string views:
/// the offline surfaces hand this to a JSON writer long after the host catalog
/// call returns, and a view into a host-owned literal is not something a caller
/// outside `pulp::host` should have to reason about. The field set is what a
/// caller needs to author a device placement the host binding will admit — the
/// binding key it must name, the domain that fixes where in the chain the
/// device is legal, and the latency it reports so the caller can explain the
/// scheduling-window shift before anything is played.
struct DeviceCatalogEntry {
    std::string binding_key;
    std::string display_name;
    std::string manufacturer;
    std::string category;
    std::string summary;
    /// `"event-to-event"` or `"event-to-audio"`. The stable public spelling of
    /// the device's domain, and the axis the chain-shape refusal is written in.
    std::string domain;
    int num_audio_inputs = 0;
    int num_audio_outputs = 0;
    int latency_samples = 0;
    bool is_instrument = false;
    std::vector<DeviceParameterCatalogEntry> parameters;
};

/// Every device the host binding can instantiate, in chain order: an
/// event-to-event device may precede an event-to-audio one, never follow it.
std::vector<DeviceCatalogEntry> device_catalog();

/// The longest device chain the host binding lowers today.
///
/// Published rather than derived, because a caller that authors a longer chain
/// is refused at admission and a refusal a client could have predicted should
/// not require a round trip to discover.
std::size_t admitted_device_chain_length() noexcept;

/// The per-chain event-compensation ceiling, in samples.
///
/// A chain whose accumulated device latency exceeds this is refused rather than
/// compensated, so a caller can add the catalog's `latency_samples` itself and
/// know the answer before authoring the placement.
int event_device_latency_ceiling_samples() noexcept;

/// Serializes the catalog and its two chain bounds as one JSON object.
///
/// The object a caller reads to discover what it may author: `devices`, plus
/// the `max_chain_length` and `latency_ceiling_samples` that bound them.
std::string device_catalog_json();

/// @}

} // namespace pulp::tools::timeline
