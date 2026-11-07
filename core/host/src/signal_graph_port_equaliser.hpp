#pragma once

// Per-output-port latency equaliser for Custom nodes that declare different
// artifact latencies on different output ports. The graph reports one scalar
// latency per node (and one total to the host), so ports with less latency are
// delayed up to the node's maximum inside the node binding. Storage is sized on
// the control thread; process() is allocation-free and lock-free.

#include <pulp/audio/buffer.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pulp::host::detail {

class OutputPortLatencyEqualiser {
  public:
    // `port_latencies` are the declared, already clamped per-port latencies.
    // Returns false (and stays inert) when every port already matches.
    bool prepare(const std::vector<int>& port_latencies) {
        port_latencies_ = port_latencies;
        const int max_latency = port_latencies.empty() ? 0
                                                       : *std::max_element(port_latencies.begin(),
                                                                           port_latencies.end());
        delays_.assign(port_latencies.size(), 0);
        rings_.assign(port_latencies.size(), {});
        positions_.assign(port_latencies.size(), 0);
        bool any = false;
        for (std::size_t port = 0; port < port_latencies.size(); ++port) {
            const auto delay = static_cast<std::size_t>(max_latency - port_latencies[port]);
            delays_[port] = delay;
            if (delay > 0) {
                rings_[port].assign(delay, 0.0f);
                any = true;
            }
        }
        return any;
    }

    const std::vector<int>& port_latencies() const noexcept {
        return port_latencies_;
    }

    // Delays each output port (one channel per port) by its equalising amount.
    void process(audio::BufferView<float>& output, int num_samples) noexcept {
        if (num_samples <= 0)
            return;
        const auto frames = std::min(static_cast<std::size_t>(num_samples), output.num_samples());
        const auto ports = std::min(output.num_channels(), delays_.size());
        for (std::size_t port = 0; port < ports; ++port) {
            const auto delay = delays_[port];
            if (delay == 0)
                continue;
            auto channel = output.channel(port);
            auto& ring = rings_[port];
            auto position = positions_[port];
            for (std::size_t i = 0; i < frames; ++i) {
                const float input = channel[i];
                channel[i] = ring[position];
                ring[position] = input;
                if (++position == delay)
                    position = 0;
            }
            positions_[port] = position;
        }
    }

  private:
    std::vector<int> port_latencies_;
    std::vector<std::size_t> delays_;
    std::vector<std::vector<float>> rings_;
    std::vector<std::size_t> positions_;
};

} // namespace pulp::host::detail
