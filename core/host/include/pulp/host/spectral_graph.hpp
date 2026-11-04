#pragma once

#include <pulp/graph/spectral_frame.hpp>
#include <pulp/host/signal_graph_node.hpp>

namespace pulp::host {

using graph::SpectralError;
using graph::SpectralIdentity;
using graph::QualifiedSpectralStage;
template <typename T> using SpectralResult = graph::SpectralResult<T>;

struct SpectralInputPort { PortHandle handle; };
struct SpectralOutputPort { PortHandle handle; };
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

} // namespace pulp::host
