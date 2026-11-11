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

/// Finite mono profile-1 source (FFT 256, hop 64). Connect a direct host
/// AudioInput to morph; each frame consumes the preceding 64 normalized samples.
/// render_frames includes the 320-sample startup delay and complete finite tail.
struct SpectralAssetMorphPorts {
    NodeId node = 0;
    PortHandle morph;
    SpectralOutputPort frames;
    std::uint64_t render_frames = 0;
};

} // namespace pulp::host
