#pragma once

#include <pulp/graph/spectral_frame.hpp>
#include <pulp/host/signal_graph_node.hpp>

namespace pulp::host {

using graph::QualifiedSpectralStage;
using graph::SpectralError;
using graph::SpectralIdentity;
template <typename T> using SpectralResult = graph::SpectralResult<T>;

struct SpectralInputPort {
    PortHandle handle;
};
struct SpectralOutputPort {
    PortHandle handle;
};
struct SpectralAnalysisPorts {
    NodeId node = 0;
    std::vector<PortHandle> audio;
    SpectralOutputPort frames;
};
struct SpectralStagePorts {
    NodeId node = 0;
    SpectralInputPort frames_in;
    SpectralOutputPort frames_out;
};
struct SpectralSynthesisPorts {
    NodeId node = 0;
    SpectralInputPort frames;
    std::vector<PortHandle> audio;
};

/// Finite profile-1 morph between two streams at their decoded geometry: FFT N a
/// power of two from 256 to 16384, hop H dividing N, 1 to 16 channels. Connect a direct host
/// AudioInput to morph; each frame consumes the preceding H normalized samples.
/// render_frames includes the N + H startup delay and complete finite tail.
struct SpectralAssetMorphPorts {
    NodeId node = 0;
    PortHandle morph;
    SpectralOutputPort frames;
    std::uint64_t render_frames = 0;
};

/// The explicit end of a finite spectral source, derived with checked
/// arithmetic from the decoded geometry (FFT N, hop H) and source length L.
/// frame_count = floor((L - 1) / H) + floor((N - 1) / H) + 1 analysis-grid
/// frames overlap [0, L); drain_frames = ceil(N / H) zero frames follow them;
/// output is silent from render_frames = (N + H) + floor((L - 1) / H) * H + N,
/// which includes the N + H startup delay. policy_version names the rule that
/// derived these values; version 2 admits every hop from 1 to N / 2.
struct SpectralFiniteStreamEnd {
    std::uint32_t policy_version = 0;
    std::uint64_t source_samples = 0;
    std::uint64_t frame_count = 0;
    std::uint64_t drain_frames = 0;
    std::uint64_t render_frames = 0;
};

/// Finite profile-1 source of one decoded stream at its own geometry: FFT N a
/// power of two from 256 to 16384, hop H from 1 to N / 2, 1 to 16 channels. It
/// has no audio input: frame f is published in the host block that reaches its
/// window end, the same sample timeline analysis would publish it on, and its
/// stored bins pass downstream unchanged.
struct SpectralAssetSourcePorts {
    NodeId node = 0;
    SpectralOutputPort frames;
    SpectralFiniteStreamEnd end;
};

} // namespace pulp::host
