#pragma once

/// @file bursty_processor.hpp
/// Negative-control fixture for the transition-cost gates.
///
/// A processor that does one 8192-point forward FFT per block in steady state
/// and, on the block where its `engage` toggle flips, `BurstIffts` extra
/// inverse FFTs each preceded by a per-bin `rt::polar` resynthesis — the
/// shape of a freeze latch that did ~23 IFFTs and ~23 × 4097 polar calls in
/// one callback. `BurstIffts = 0` is the well-behaved twin: it does exactly
/// the steady work on the edge block. A transition gate that cannot fail on
/// `BurstyProcessor<23>` and pass on `BurstyProcessor<0>` is not measuring
/// transition cost.

#include <pulp/format/processor.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/rt_work_counter.hpp>

#include <complex>
#include <memory>
#include <vector>

namespace pulp::test::audio {

enum BurstyParams : pulp::state::ParamID {
    kBurstyEngage = 1,
    kBurstyGain = 2,
};

template <int BurstIffts>
class BurstyProcessor final : public pulp::format::Processor {
public:
    static constexpr int kFftSize = 8192;
    static constexpr int kBins = kFftSize / 2 + 1;

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "BurstyProcessor",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.bursty",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kBurstyEngage,
            .name = "Engage",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
            .kind = pulp::state::ParamKind::Toggle,
        });
        store.add_parameter({
            .id = kBurstyGain,
            .name = "Gain",
            .range = {0.0f, 1.0f, 1.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {
        fft_ = std::make_unique<pulp::signal::Fft>(kFftSize);
        frame_.assign(kFftSize, {});
        primed_ = false;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const float gain = state().get_value(kBurstyGain);
        const bool engaged = state().get_value(kBurstyEngage) >= 0.5f;

        // Steady work: one analysis FFT of the newest input.
        const auto in0 = input.num_channels() > 0 ? input.channel(0)
                                                  : std::span<const float>{};
        for (int i = 0; i < kFftSize; ++i)
            frame_[i] = {i < static_cast<int>(in0.size()) ? in0[i] : 0.0f, 0.0f};
        fft_->forward(frame_.data());

        // The first block adopts the initial state; only a later change is
        // an edge.
        if (!primed_) {
            primed_ = true;
            engaged_ = engaged;
        }
        if (engaged != engaged_) {
            engaged_ = engaged;
            for (int burst = 0; burst < BurstIffts; ++burst) {
                for (int k = 0; k < kBins; ++k)
                    frame_[k] = pulp::signal::rt::polar(
                        std::abs(frame_[k]), 0.001f * static_cast<float>(k + burst));
                fft_->inverse(frame_.data());
            }
        }

        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            const auto in = ch < input.num_channels() ? input.channel(ch)
                                                      : std::span<const float>{};
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] = (i < in.size() ? in[i] : 0.0f) * gain;
        }
    }

private:
    std::unique_ptr<pulp::signal::Fft> fft_;
    std::vector<std::complex<float>> frame_;
    bool engaged_ = false;
    bool primed_ = false;
};

template <int BurstIffts>
std::unique_ptr<pulp::format::Processor> make_bursty_processor() {
    return std::make_unique<BurstyProcessor<BurstIffts>>();
}

} // namespace pulp::test::audio
