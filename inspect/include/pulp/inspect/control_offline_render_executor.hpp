#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/format/offline_render_host.hpp>
#include <pulp/inspect/control_execution.hpp>

#include <cstdint>
#include <chrono>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace pulp::inspect {

/// Owned, transport-independent input resolved by a trusted T0 launcher from
/// the request's opaque input artifact ID. No path from untrusted request JSON
/// reaches the filesystem or selects a ProcessorFactory.
struct ControlOfflineRenderSource {
    format::OfflineRenderConfig config;
    audio::Buffer<float> input;
    std::vector<format::OfflineMidiEvent> midi_events;
    std::vector<format::OfflineParameterEvent> parameter_events;
    std::uint64_t frame_count = 0;
    std::uint32_t block_frames = 0;
};

using ControlOfflineRenderSourceResolver =
    std::function<std::optional<ControlOfflineRenderSource>(
        const ControlAdmissionPlan&, std::string_view input_artifact_id)>;
using ControlOfflineRenderClock =
    std::function<std::chrono::steady_clock::time_point()>;

/// Creates the canonical dev.pulp.render/offline@1 T0 executor.
///
/// The returned adapter performs no listening, registration, granting, or
/// production activation. ControlService admits the operation and binds
/// publish_artifact to the running receipt; this adapter only resolves trusted
/// in-memory input, drives OfflineRenderHost, and publishes a float32 WAV.
ControlOperationExecutor
make_control_offline_render_executor(format::ProcessorFactory factory,
                                     ControlOfflineRenderSourceResolver resolve_source,
                                     ControlOfflineRenderClock clock = {});

} // namespace pulp::inspect
