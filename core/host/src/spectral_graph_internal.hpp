#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/signal_graph_connection.hpp>
#include <pulp/host/spectral_graph.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <array>
#include <limits>
#include <memory>

namespace pulp::host {

class SpectralRuntimeBank {
  public:
    static constexpr std::size_t memory_limit = 64u * 1024u * 1024u;
    struct Edge {
        std::vector<graph::SpectralFrameStamp> stamps;
        std::vector<std::complex<float>> bins;
        graph::SpectralFrameBatchView view;
    };
    struct Node {
        NodeId id;
        NodeId root = 0;
        NodeType type;
        SpectralIdentity identity;
        graph::SpectralFrameContract contract;
        std::optional<QualifiedSpectralStage> stage;
        std::unique_ptr<signal::SpectralFrameEngine> engine;
        std::vector<std::complex<float>> working;
        graph::SpectralFrameBatchView* input = nullptr;
        std::vector<graph::SpectralFrameBatchView*> outputs;
        std::uint64_t epoch = 0, sequence = 0;
        std::int64_t origin = 0, output_clock = 0;
        std::int64_t fatal_origin = std::numeric_limits<std::int64_t>::max();
        SpectralError first_error = SpectralError::None;
        std::uint64_t errors = 0;
        std::uint64_t first_error_sequence = 0;
        std::int64_t first_error_position = 0;

        Node(const GraphNode& n)
            : id(n.id), type(n.type), identity(*n.spectral_identity),
              contract(identity.frame_contract()), stage(n.spectral_stage) {}
        void error(SpectralError e) noexcept {
            if (first_error == SpectralError::None) {
                first_error = e;
                first_error_sequence = sequence;
                first_error_position = origin;
            }
            if (errors != std::numeric_limits<std::uint64_t>::max())
                ++errors;
        }
        void fatal(std::int64_t at, SpectralError e) noexcept {
            error(e);
            fatal_origin = std::min(fatal_origin, at);
            for (auto* out : outputs) {
                out->fatal = true;
                out->fatal_origin = fatal_origin;
            }
        }
        bool advance() noexcept {
            if (sequence == std::numeric_limits<std::uint64_t>::max() ||
                origin > std::numeric_limits<std::int64_t>::max() - identity.fft_size() -
                             identity.hop()) {
                fatal(origin, SpectralError::ChronologyExhausted);
                return false;
            }
            ++sequence;
            origin += identity.hop();
            return true;
        }
        bool valid_bins(std::span<const std::complex<float>> bins) const noexcept {
            if (bins.size() != working.size())
                return false;
            for (auto b : bins)
                if (!std::isfinite(b.real()) || !std::isfinite(b.imag()))
                    return false;
            for (int c = 0; c < identity.channels(); ++c)
                if (bins[c * identity.bins()].imag() != 0 ||
                    bins[(c + 1) * identity.bins() - 1].imag() != 0)
                    return false;
            return true;
        }
        void publish() noexcept {
            for (auto* out : outputs) {
                if (out->count == out->stamps.size()) {
                    fatal(origin, SpectralError::CapacityExceeded);
                    return;
                }
            }
            for (auto* out : outputs) {
                out->contract = contract;
                out->encoded_contract = contract.encode();
                out->stamps[out->count] = {epoch, sequence, origin, origin + identity.fft_size()};
                std::copy(working.begin(), working.end(),
                          out->bins.begin() + out->count * working.size());
                ++out->count;
            }
        }
        static bool process(format::ProcessBlock&,
                            const format::GraphRuntimeNodeProcessContext& ctx, void* raw) noexcept {
            auto& n = *static_cast<Node*>(raw);
            auto output_view = ctx.node_outputs;
            const int count = static_cast<int>(ctx.node_outputs.num_samples());
            const int channels = n.identity.channels(), bins = n.identity.bins();
            const auto limit = std::numeric_limits<std::int64_t>::max();
            if (n.output_clock > limit - count - n.identity.fft_size() - n.identity.hop() ||
                n.origin > limit - n.identity.fft_size() - n.identity.hop())
                n.fatal(std::min(n.origin, limit - n.identity.fft_size() - n.identity.hop()),
                        SpectralError::ChronologyExhausted);
            if (n.type == NodeType::SpectralAnalysis && n.fatal_origin == limit) {
                std::array<const float*, 16> inputs{};
                for (int c = 0; c < channels; ++c)
                    inputs[c] = ctx.node_inputs.channel_ptr(c);
                const auto result = n.engine->analyze_guarded(
                    inputs.data(), count, [&](std::complex<float>* const* frame, int) {
                        if (n.fatal_origin != limit)
                            return;
                        for (int c = 0; c < channels; ++c)
                            std::copy_n(frame[c], bins, n.working.begin() + c * bins);
                        if (!n.valid_bins(n.working)) {
                            n.error(SpectralError::InvalidFrame);
                            std::fill(n.working.begin(), n.working.end(), std::complex<float>{});
                        }
                        n.publish();
                        n.advance();
                    });
                if (result != signal::SpectralSplitResult::Ok)
                    n.fatal(n.origin, SpectralError::ChronologyExhausted);
            } else if (n.type != NodeType::SpectralAnalysis && n.fatal_origin == limit) {
                const auto* batch = ctx.spectral_input;
                const auto decoded_contract = batch
                                                  ? graph::SpectralFrameContract::decode(
                                                        batch->encoded_contract)
                                                  : graph::SpectralResult<graph::SpectralFrameContract>{};
                if (!batch || !decoded_contract || *decoded_contract.value != n.contract ||
                    batch->contract != *decoded_contract.value) {
                    n.error(SpectralError::IdentityMismatch);
                    return false;
                }
                std::uint32_t cursor = 0;
                while (n.origin + n.identity.fft_size() <= n.output_clock + count) {
                    if (batch->fatal && n.origin >= batch->fatal_origin) {
                        if (n.type == NodeType::SpectralSynthesis)
                            n.engine->stop_synthesis_guarded();
                        n.fatal(batch->fatal_origin, SpectralError::CapacityExceeded);
                        break;
                    }
                    while (cursor < batch->count && batch->stamps[cursor].sequence < n.sequence) {
                        n.error(SpectralError::InvalidFrame);
                        ++cursor;
                    }
                    const graph::SpectralFrameStamp expected{n.epoch, n.sequence, n.origin,
                                                             n.origin + n.identity.fft_size()};
                    bool valid = cursor < batch->count && batch->stamps[cursor] == expected;
                    if (valid) {
                        auto payload =
                            batch->bins.subspan(cursor * n.working.size(), n.working.size());
                        valid = n.valid_bins(payload);
                        if (valid)
                            std::copy(payload.begin(), payload.end(), n.working.begin());
                    }
                    if (cursor < batch->count && batch->stamps[cursor].sequence == n.sequence)
                        ++cursor;
                    if (!valid) {
                        n.error(SpectralError::MissingFrame);
                        std::fill(n.working.begin(), n.working.end(), std::complex<float>{});
                    }
                    if (n.type == NodeType::SpectralStage) {
                        const bool zero =
                            std::all_of(n.working.begin(), n.working.end(),
                                        [](auto value) { return value == std::complex<float>{}; });
                        switch (n.stage->kind()) {
                        case QualifiedSpectralStage::Kind::IdentityV1:
                            break;
                        case QualifiedSpectralStage::Kind::RealMaskV1:
                            for (size_t k = 0; k < n.working.size(); ++k)
                                n.working[k] *= n.stage->gains()[k];
                            break;
                        }
                        if (!n.valid_bins(n.working) ||
                            (zero &&
                             std::any_of(n.working.begin(), n.working.end(), [](auto value) {
                                 return value != std::complex<float>{};
                             }))) {
                            n.error(SpectralError::StageContractViolation);
                            std::fill(n.working.begin(), n.working.end(), std::complex<float>{});
                        }
                        n.publish();
                    } else {
                        std::array<const std::complex<float>*, 16> frames{};
                        for (int c = 0; c < channels; ++c)
                            frames[c] = n.working.data() + c * bins;
                        if (n.engine->synthesize_frame_guarded(frames.data()) !=
                            signal::SpectralSplitResult::Ok) {
                            n.engine->stop_synthesis_guarded();
                            n.fatal(n.origin, SpectralError::SynthesisOverflow);
                            break;
                        }
                    }
                    if (!n.advance())
                        break;
                }
                // Unsolicited trailing records never create another scheduled
                // slot, including duplicates arriving in a zero-frame block.
                while (n.fatal_origin == limit && cursor < batch->count) {
                    n.error(SpectralError::InvalidFrame);
                    ++cursor;
                }
            }
            if (n.fatal_origin != limit) {
                for (auto* out : n.outputs) {
                    out->fatal = true;
                    out->fatal_origin = n.fatal_origin;
                }
            }
            if (n.type == NodeType::SpectralSynthesis) {
                const auto latency = n.identity.fft_size() + n.identity.hop();
                const auto cutoff = n.fatal_origin == limit
                                        ? limit
                                        : std::max<std::int64_t>(0, n.fatal_origin) + latency;
                const int prefix =
                    static_cast<int>(std::clamp<std::int64_t>(latency - n.output_clock, 0, count));
                for (int c = 0; c < channels; ++c)
                    std::fill_n(output_view.channel_ptr(c), count, 0.0f);
                const int end = static_cast<int>(
                    std::clamp<std::int64_t>(cutoff - n.output_clock, prefix, count));
                std::array<float*, 16> outputs{};
                for (int c = 0; c < channels; ++c)
                    outputs[c] = output_view.channel_ptr(c) + prefix;
                int consumed = 0;
                if (n.engine->read_output_guarded(outputs.data(), end - prefix, consumed) !=
                    signal::SpectralSplitResult::Ok)
                    n.fatal(n.output_clock + prefix + consumed - latency,
                            SpectralError::OutputUnderrun);
            }
            if (n.output_clock <= limit - count)
                n.output_clock += count;
            return true;
        }
    };

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Edge>> edges;
    std::size_t charged_bytes = 0;
    SpectralError prepare(std::span<const GraphNode>, std::span<const Connection>, double, int);
    void begin_block() noexcept {
        for (auto& edge : edges) {
            edge->view.count = 0;
            edge->view.fatal = false;
        }
    }
    void reset(std::uint64_t epoch) noexcept {
        for (auto& node : nodes) {
            if (node->engine)
                node->engine->reset();
            node->epoch = epoch;
            node->sequence = 0;
            node->origin = signal::spectral_frame_engine_first_frame_start(
                node->identity.fft_size(), node->identity.hop(), true);
            node->output_clock = 0;
            node->fatal_origin = std::numeric_limits<std::int64_t>::max();
            node->first_error = SpectralError::None;
            node->errors = 0;
            node->first_error_sequence = 0;
            node->first_error_position = 0;
        }
        begin_block();
    }
    Node* find(NodeId id) const noexcept {
        for (const auto& n : nodes)
            if (n->id == id)
                return n.get();
        return nullptr;
    }
    format::GraphRuntimeNodeBinding binding(NodeId id) noexcept {
        auto* n = find(id);
        if (!n)
            return {};
        format::GraphRuntimeNodeBinding binding{id, &Node::process, n, true};
        binding.spectral_input = n->input;
        binding.spectral_outputs = n->outputs;
        binding.reads_inputs_only = true;
        return binding;
    }
};
} // namespace pulp::host
