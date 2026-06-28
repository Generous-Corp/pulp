// render_scenario.cpp — typed offline render scenarios.
// Event-application semantics are documented in render_scenario.hpp; keep
// the block loop here in lock-step with those docs.

#include "render_scenario.hpp"

#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulp::test::audio {

namespace {

void require(bool ok, const std::string& what) {
    if (!ok)
        throw std::invalid_argument("RenderScenario: " + what);
}

} // namespace

RenderScenario& RenderScenario::input(pulp::audio::Buffer<float> buffer) {
    input_channels_ = static_cast<int>(buffer.num_channels());
    fixed_input_ = std::move(buffer);
    has_fixed_input_ = true;
    input_generator_ = nullptr;
    return *this;
}

std::int64_t RenderScenario::resolve_duration_frames() const {
    if (duration_frames_ > 0)
        return duration_frames_;
    if (duration_ms_ > 0.0)
        return static_cast<std::int64_t>(
            std::llround(duration_ms_ * sample_rate_ / 1000.0));
    if (has_fixed_input_ && fixed_input_.num_samples() > 0)
        return static_cast<std::int64_t>(fixed_input_.num_samples());
    return -1;
}

ScenarioResult RenderScenario::render() const {
    require(factory_ != nullptr, "no processor factory");
    require(sample_rate_ > 0.0, "sample rate must be > 0");
    require(block_size_ > 0, "block size must be > 0");
    require(input_channels_ >= 0 && output_channels_ > 0,
            "invalid channel layout");
    const std::int64_t total = resolve_duration_frames();
    require(total > 0, "no duration (set duration_frames/duration_ms or a "
                       "fixed input buffer)");

    // Materialize the input stimulus at full render length. A fixed input
    // shorter than the render is zero-padded (silence after it ends).
    pulp::audio::Buffer<float> input(
        static_cast<std::size_t>(input_channels_),
        static_cast<std::size_t>(total));
    if (input_generator_) {
        auto generated = input_generator_(sample_rate_, input_channels_, total);
        require(static_cast<int>(generated.num_channels()) == input_channels_ &&
                    static_cast<std::int64_t>(generated.num_samples()) == total,
                "input generator returned a mismatched buffer shape");
        input = std::move(generated);
    } else if (has_fixed_input_) {
        const auto frames = std::min<std::size_t>(fixed_input_.num_samples(),
                                                  static_cast<std::size_t>(total));
        for (std::size_t ch = 0; ch < input.num_channels(); ++ch) {
            auto src = fixed_input_.channel(ch);
            std::copy_n(src.begin(), frames, input.channel(ch).begin());
        }
    }

    pulp::format::HeadlessHost host(factory_);
    host.prepare(sample_rate_, block_size_, input_channels_, output_channels_);
    for (const auto& [id, value] : initial_params_)
        host.state().set_value(id, value);

    // Stable-sort scripts by frame so equal-frame events keep script order.
    auto midi_script = midi_script_;
    std::stable_sort(midi_script.begin(), midi_script.end(),
                     [](const auto& a, const auto& b) { return a.frame < b.frame; });
    auto param_script = param_script_;
    std::stable_sort(param_script.begin(), param_script.end(),
                     [](const auto& a, const auto& b) { return a.frame < b.frame; });

    ScenarioResult result;
    result.sample_rate = sample_rate_;
    result.block_size = block_size_;
    result.output.resize(static_cast<std::size_t>(output_channels_),
                         static_cast<std::size_t>(total));

    const auto input_view = std::as_const(input).view();
    std::vector<float*> out_ptrs(static_cast<std::size_t>(output_channels_));
    std::size_t midi_idx = 0, param_idx = 0;

    for (std::int64_t pos = 0; pos < total; pos += block_size_) {
        const auto n = static_cast<std::size_t>(
            std::min<std::int64_t>(block_size_, total - pos));

        // Parameter steps in [pos, pos+n): block-quantized (see header).
        for (; param_idx < param_script.size() &&
               param_script[param_idx].frame < pos + static_cast<std::int64_t>(n);
             ++param_idx) {
            const auto& step = param_script[param_idx];
            host.state().set_value(step.id, step.value);
        }

        // MIDI events in [pos, pos+n): sample-accurate per-block offsets.
        pulp::midi::MidiBuffer midi_in, midi_out;
        for (; midi_idx < midi_script.size() &&
               midi_script[midi_idx].frame < pos + static_cast<std::int64_t>(n);
             ++midi_idx) {
            auto event = midi_script[midi_idx].event;
            event.sample_offset = static_cast<std::int32_t>(
                std::max<std::int64_t>(midi_script[midi_idx].frame - pos, 0));
            midi_in.add(event);
        }

        auto in_view = input_view.slice(static_cast<std::size_t>(pos), n);
        for (std::size_t ch = 0; ch < out_ptrs.size(); ++ch)
            out_ptrs[ch] = result.output.channel(ch).data() +
                           static_cast<std::size_t>(pos);
        pulp::audio::BufferView<float> out_view(out_ptrs.data(),
                                                out_ptrs.size(), n);
        host.process(out_view, in_view, midi_in, midi_out);
    }

    result.metrics = analyze(result.output, sample_rate_);
    std::ostringstream provenance;
    provenance << name_ << " sr=" << sample_rate_ << " block=" << block_size_
               << " in=" << input_channels_ << " out=" << output_channels_
               << " frames=" << total;
    result.scenario = provenance.str();
    return result;
}

std::vector<MatrixCell> run_matrix(const RenderScenario& scenario,
                                   std::span<const double> sample_rates,
                                   std::span<const int> block_sizes) {
    std::vector<MatrixCell> cells;
    cells.reserve(sample_rates.size() * block_sizes.size());
    for (double sr : sample_rates) {
        for (int block : block_sizes) {
            auto cell_scenario = scenario;
            cell_scenario.sample_rate(sr).block_size(block);
            cells.push_back({sr, block, cell_scenario.render()});
        }
    }
    return cells;
}

CheckResult assert_block_partition_invariant(const RenderScenario& scenario,
                                             std::span<const int> block_sizes,
                                             double tolerance_dbfs) {
    if (block_sizes.size() < 2)
        return {false, "block partition invariance needs >= 2 block sizes"};

    auto reference_scenario = scenario;
    const auto reference =
        reference_scenario.block_size(block_sizes.front()).render();

    for (std::size_t i = 1; i < block_sizes.size(); ++i) {
        auto candidate_scenario = scenario;
        const auto candidate =
            candidate_scenario.block_size(block_sizes[i]).render();
        auto null_check = assert_null_near(reference.output, candidate.output,
                                           tolerance_dbfs);
        if (!null_check.passed) {
            std::ostringstream msg;
            msg << "block partition variance between block="
                << block_sizes.front() << " and block=" << block_sizes[i]
                << " (" << reference.scenario << "): " << null_check.message;
            return {false, msg.str()};
        }
    }
    std::ostringstream msg;
    msg << "partition-invariant across " << block_sizes.size()
        << " block sizes (" << reference.scenario << "), residual tolerance "
        << tolerance_dbfs << " dBFS";
    return {true, msg.str()};
}

} // namespace pulp::test::audio
