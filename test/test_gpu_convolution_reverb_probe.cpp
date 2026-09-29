#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/forge_space_catalog.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace {
using pulp::host::BakedParamView;
using pulp::state::ParamID;
constexpr double kSr = 48000.0;
constexpr int kQ = 128;
struct Params final : BakedParamView {
    float ir_gain = 0.0f, predelay = 0.0f, wet = 100.0f, dry = 0.0f;
    float width = 100.0f, lowcut = 20.0f, highcut = 20000.0f;
    Params() = default;
    Params(float gain, float delay, float wet_percent, float dry_percent, float width_percent,
           float lowcut_hz, float highcut_hz)
        : ir_gain(gain), predelay(delay), wet(wet_percent), dry(dry_percent), width(width_percent),
          lowcut(lowcut_hz), highcut(highcut_hz) {}
    float value_at(ParamID id, int32_t) const override {
        return value(id);
    }
    float value(ParamID id) const override {
        namespace C = pulp::host::space::convolution;
        switch (id) {
        case C::kIrGainDb:
            return ir_gain;
        case C::kPredelayMs:
            return predelay;
        case C::kWetPercent:
            return wet;
        case C::kDryPercent:
            return dry;
        case C::kWidthPercent:
            return width;
        case C::kLowcutHz:
            return lowcut;
        case C::kHighcutHz:
            return highcut;
        default:
            return 0.0f;
        }
    }
};
struct ParamEvent {
    int sample = 0;
    Params values;
};
struct ParamTimeline {
    Params initial;
    std::vector<ParamEvent> events;

    Params at(int sample) const {
        Params result = initial;
        for (const auto& event : events) {
            if (event.sample > sample)
                break;
            result = event.values;
        }
        return result;
    }
};
using NodeType = pulp::host::CustomNodeType;
using IR = pulp::host::space::convolution::ImpulseResponse;
struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};
IR delta_ir_44100() {
    IR ir;
    ir.sample_rate = 44100.0;
    ir.channels.assign(2, std::vector<float>(32, 0.0f));
    ir.channels[0][0] = 1.0f;
    ir.channels[0][2] = 0.25f;
    ir.channels[1][0] = 0.75f;
    ir.channels[1][3] = -0.5f;
    return ir;
}
float stimulus_sample(int sample, int channel) {
    const double t = static_cast<double>(sample);
    const double phase = channel == 0 ? 0.17 : 0.53;
    const double value = 0.031 * std::sin(0.071 * t + phase) +
                         0.019 * std::cos(0.137 * t + 0.31 * channel) +
                         0.011 * std::sin(0.013 * t + 0.79 * channel);
    return static_cast<float>(value);
}
Stereo run(NodeType& type, const std::vector<int>& pattern, const ParamTimeline& timeline,
           int max_block, int total, bool inplace = false) {
    void* instance = type.create();
    if (!instance)
        throw std::runtime_error("GPU factory returned null instance");
    bool prepared = false;
    try {
        type.prepare(instance, kSr, max_block);
        prepared = true;
        Stereo result;
        result.left.reserve(static_cast<std::size_t>(total));
        result.right.reserve(static_cast<std::size_t>(total));
        int offset = 0;
        std::size_t cursor = 0;
        while (offset < total) {
            const int n = std::min(pattern[cursor++ % pattern.size()], total - offset);
            std::vector<float> left(static_cast<std::size_t>(n), 0.0f);
            std::vector<float> right(static_cast<std::size_t>(n), 0.0f);
            for (int i = 0; i < n; ++i) {
                left[static_cast<std::size_t>(i)] = stimulus_sample(offset + i, 0);
                right[static_cast<std::size_t>(i)] = stimulus_sample(offset + i, 1);
            }
            if (offset == 0) {
                left[0] += 1.0f;
                right[0] += 0.3f;
            }
            std::vector<float> out_l =
                inplace ? left : std::vector<float>(static_cast<std::size_t>(n), 0.0f);
            std::vector<float> out_r =
                inplace ? right : std::vector<float>(static_cast<std::size_t>(n), 0.0f);
            float* outputs[2] = {out_l.data(), out_r.data()};
            const float* inputs[2] = {inplace ? out_l.data() : left.data(),
                                      inplace ? out_r.data() : right.data()};
            pulp::audio::BufferView<const float> input(inputs, 2, n);
            pulp::audio::BufferView<float> output(outputs, 2, n);
            const auto params = timeline.at(offset);
            type.process_instance_baked_param(instance, output, input, n, params);
            result.left.insert(result.left.end(), out_l.begin(), out_l.end());
            result.right.insert(result.right.end(), out_r.begin(), out_r.end());
            offset += n;
        }
        if (type.release)
            type.release(instance);
        type.destroy(instance);
        return result;
    } catch (...) {
        if (prepared && type.release)
            type.release(instance);
        type.destroy(instance);
        throw;
    }
}
void require(bool ok, const std::string& message);
Stereo cpu_oracle(const std::vector<int>& pattern, const ParamTimeline& timeline, int max_block,
                  int total) {
    const auto ir = delta_ir_44100();
    pulp::signal::ZeroLatencyConvolver cpu;
    cpu.prepare(kSr, max_block, 2);
    cpu.set_normalize_mode(pulp::signal::IrNormalizeMode::energy);
    cpu.set_tail_trim_db(pulp::signal::ZeroLatencyConvolver::kTailTrimDbDefault);
    cpu.set_tail_fade_ms(pulp::signal::ZeroLatencyConvolver::kTailFadeMsDefault);
    cpu.set_resample_taps_per_phase(pulp::signal::ZeroLatencyConvolver::kResampTapsPerPhaseDefault);
    const float* taps[2] = {ir.channels[0].data(), ir.channels[1].data()};
    if (!cpu.load_impulse_response(taps, 2, static_cast<int>(ir.channels[0].size()),
                                   ir.sample_rate))
        throw std::runtime_error("CPU oracle IR load failed");
    Stereo result;
    result.left.reserve(static_cast<std::size_t>(total));
    result.right.reserve(static_cast<std::size_t>(total));
    int offset = 0;
    std::size_t cursor = 0;
    while (offset < total) {
        const int n = std::min(pattern[cursor++ % pattern.size()], total - offset);
        std::vector<float> in_l(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> in_r(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> out_l(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> out_r(static_cast<std::size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i) {
            in_l[static_cast<std::size_t>(i)] = stimulus_sample(offset + i, 0);
            in_r[static_cast<std::size_t>(i)] = stimulus_sample(offset + i, 1);
        }
        if (offset == 0) {
            in_l[0] += 1.0f;
            in_r[0] += 0.3f;
        }
        const float* inputs[2] = {in_l.data(), in_r.data()};
        float* outputs[2] = {out_l.data(), out_r.data()};
        const auto params = timeline.at(offset);
        cpu.set_ir_gain_db(params.ir_gain);
        cpu.set_predelay_ms(params.predelay);
        cpu.set_wet_percent(params.wet);
        cpu.set_dry_percent(params.dry);
        cpu.set_width_percent(params.width);
        cpu.set_lowcut_hz(params.lowcut);
        cpu.set_highcut_hz(params.highcut);
        cpu.process(inputs, outputs, n);
        result.left.insert(result.left.end(), out_l.begin(), out_l.end());
        result.right.insert(result.right.end(), out_r.begin(), out_r.end());
        offset += n;
    }
    return result;
}

void compare_aligned(const Stereo& route, const Stereo& cpu, int latency,
                     const std::string& label) {
    require(route.left.size() == cpu.left.size(), label + " left length mismatch");
    require(route.right.size() == cpu.right.size(), label + " right length mismatch");
    for (std::size_t i = 0; i < route.left.size(); ++i) {
        const float expected_l = i < static_cast<std::size_t>(latency)
                                     ? 0.0f
                                     : cpu.left[i - static_cast<std::size_t>(latency)];
        const float expected_r = i < static_cast<std::size_t>(latency)
                                     ? 0.0f
                                     : cpu.right[i - static_cast<std::size_t>(latency)];
        if (!std::isfinite(route.left[i]) || !std::isfinite(route.right[i]) ||
            !std::isfinite(expected_l) || !std::isfinite(expected_r) ||
            std::fabs(route.left[i] - expected_l) > 2.0e-4f ||
            std::fabs(route.right[i] - expected_r) > 2.0e-4f)
            throw std::runtime_error(label + " diverged from CPU oracle at sample " +
                                     std::to_string(i));
    }
}
int first_nonzero(const std::vector<float>& x) {
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > 1.0e-5f)
            return static_cast<int>(i);
    return -1;
}
float max_abs_difference(const Stereo& lhs, const Stereo& rhs) {
    require(lhs.left.size() == rhs.left.size() && lhs.right.size() == rhs.right.size(),
            "stereo comparison length mismatch");
    float result = 0.0f;
    for (std::size_t i = 0; i < lhs.left.size(); ++i) {
        result = std::max(result, std::fabs(lhs.left[i] - rhs.left[i]));
        result = std::max(result, std::fabs(lhs.right[i] - rhs.right[i]));
    }
    return result;
}
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        const auto probe_ir = delta_ir_44100();
        pulp::gpu_audio::GpuConvolutionReverbConfig gpu_config;
        gpu_config.block_size = kQ;
        gpu_config.sample_rate = static_cast<std::uint32_t>(kSr);
        gpu_config.impulse_response_sample_rate = probe_ir.sample_rate;
        gpu_config.impulse_response = probe_ir.channels;
        gpu_config.gpu_enabled = true;
        pulp::gpu_audio::GpuConvolutionReverb direct(gpu_config);
        require(direct.prepare(), "direct GPU route prepare failed");
        const auto prepared_report = direct.report();
        require(prepared_report.authenticated_shared_provider,
                "direct GPU route did not authenticate shared provider");
        require(prepared_report.latency_samples == 3 * kQ,
                "direct GPU route latency report mismatch");
        // Preparation proves the exact Dawn provider and shared-memory lane,
        // but it does not prove that a callback selected a completed GPU
        // result.  Pace a real callback stream at the host sample rate, then
        // take the delivery snapshot only after the callback loop has stopped
        // (DeliverySnapshot is approximate while process() is running).
        constexpr int paced_blocks = 64;
        const int paced_total = paced_blocks * kQ;
        Stereo paced_result;
        paced_result.left.reserve(static_cast<std::size_t>(paced_total));
        paced_result.right.reserve(static_cast<std::size_t>(paced_total));
        std::vector<float> paced_in_l(kQ, 0.0f), paced_in_r(kQ, 0.0f);
        std::vector<float> paced_out_l(kQ, 0.0f), paced_out_r(kQ, 0.0f);
        const auto paced_start = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        for (int block = 0; block < paced_blocks; ++block) {
            const auto due = paced_start + std::chrono::nanoseconds(
                                               static_cast<std::int64_t>(block) * kQ *
                                               1'000'000'000ll / static_cast<std::int64_t>(kSr));
            std::this_thread::sleep_until(due);
            const int offset = block * kQ;
            for (int i = 0; i < kQ; ++i) {
                paced_in_l[static_cast<std::size_t>(i)] = stimulus_sample(offset + i, 0);
                paced_in_r[static_cast<std::size_t>(i)] = stimulus_sample(offset + i, 1);
            }
            if (block == 0) {
                paced_in_l[0] += 1.0f;
                paced_in_r[0] += 0.3f;
            }
            std::fill(paced_out_l.begin(), paced_out_l.end(), 0.0f);
            std::fill(paced_out_r.begin(), paced_out_r.end(), 0.0f);
            const float* paced_inputs[2] = {paced_in_l.data(), paced_in_r.data()};
            float* paced_outputs[2] = {paced_out_l.data(), paced_out_r.data()};
            const pulp::audio::BufferView<const float> paced_input(paced_inputs, 2, kQ);
            pulp::audio::BufferView<float> paced_output(paced_outputs, 2, kQ);
            direct.process(paced_input, paced_output, kQ);
            paced_result.left.insert(paced_result.left.end(), paced_out_l.begin(),
                                     paced_out_l.end());
            paced_result.right.insert(paced_result.right.end(), paced_out_r.begin(),
                                      paced_out_r.end());
        }
        const auto paced_report = direct.report();
        for (const auto& lane : paced_report.lanes)
            require(lane.delivery.gpu_blocks > 0,
                    "paced direct GPU route selected no completed GPU delivery");
        ParamTimeline direct_defaults{{}, {}};
        compare_aligned(paced_result, cpu_oracle({kQ}, direct_defaults, kQ, paced_total), 3 * kQ,
                        "paced direct GPU CPU oracle");
        const auto direct_gpu_left = paced_report.lanes[0].delivery.gpu_blocks;
        const auto direct_gpu_right = paced_report.lanes[1].delivery.gpu_blocks;
        direct.release();
        auto type = pulp::host::space::convolution::make_gpu_convolution_reverb_node(probe_ir);
        require(type.is_valid_registration(), "GPU Forge registration invalid");
        require(type.latency_samples_for_block(kSr, kQ) == 3 * kQ, "full-quantum PDC mismatch");
        require(type.latency_samples_for_block(kSr, 192) == 3 * 256,
                "non-power-of-two capacity mismatch");
        // Exercise the non-power-of-two host capacity through the real factory,
        // not only its metadata callback. The engine rounds 192 up to a 256
        // sample internal quantum while preserving arbitrary host partitions.
        ParamTimeline nonpower{{3.0f, 0.0f, 70.0f, 30.0f, 160.0f, 20.0f, 12000.0f}, {}};
        constexpr int nonpower_total = 6 * 192;
        const auto nonpower_route =
            run(type, {191, 1, 64, 192, 17, 83}, nonpower, 192, nonpower_total);
        compare_aligned(nonpower_route,
                        cpu_oracle({191, 1, 64, 192, 17, 83}, nonpower, 192, nonpower_total),
                        3 * 256, "non-power-of-two capacity CPU oracle");
        ParamTimeline dry{{}, {}};
        dry.initial.wet = 0.0f;
        dry.initial.dry = 100.0f;
        const int total = 8 * kQ;
        const auto full = run(type, {kQ}, dry, kQ, total);
        const auto single = run(type, {1}, dry, kQ, total);
        const auto irregular = run(type, {kQ - 1, 2, 17, 31, 7, 53}, dry, kQ, total);
        const auto inplace = run(type, {kQ - 1, 2, 17, 31, 7, 53}, dry, kQ, total, true);
        require(first_nonzero(full.left) == 3 * kQ, "full-quantum dry impulse PDC mismatch");
        require(first_nonzero(single.left) == 3 * kQ, "single-sample dry impulse PDC mismatch");
        require(first_nonzero(irregular.left) == 3 * kQ, "irregular dry impulse PDC mismatch");
        require(first_nonzero(inplace.left) == 3 * kQ, "in-place dry impulse PDC mismatch");
        for (int i = 0; i < total; ++i) {
            require(std::fabs(full.left[static_cast<std::size_t>(i)] -
                              single.left[static_cast<std::size_t>(i)]) < 1.0e-5f,
                    "single-sample partition changed dry output");
            require(std::fabs(full.left[static_cast<std::size_t>(i)] -
                              irregular.left[static_cast<std::size_t>(i)]) < 1.0e-5f,
                    "irregular partition changed dry output");
            require(std::fabs(irregular.left[static_cast<std::size_t>(i)] -
                              inplace.left[static_cast<std::size_t>(i)]) < 1.0e-5f,
                    "in-place processing changed output");
        }
        compare_aligned(irregular, cpu_oracle({kQ - 1, 2, 17, 31, 7, 53}, dry, kQ, total), 3 * kQ,
                        "dry CPU oracle");
        ParamTimeline wet{{}, {}};
        wet.initial.wet = 100.0f;
        wet.initial.dry = 0.0f;
        const auto wet_full = run(type, {kQ}, wet, kQ, total);
        require(first_nonzero(wet_full.left) == 3 * kQ, "native-rate wet impulse PDC mismatch");
        compare_aligned(wet_full, cpu_oracle({kQ}, wet, kQ, total), 3 * kQ, "wet CPU oracle");
        // Exercise block-boundary automation at known absolute sample
        // offsets.  These offsets are callback boundaries in the irregular
        // partition below, so the Forge block-rate baked-param contract is
        // tested without pretending that the catalog applies mid-callback
        // events.  In particular the predelay changes after prior wet history
        // exists, which catches history being applied on the convolution send.
        ParamTimeline automated{Params{0.0f, 0.0f, 75.0f, 25.0f, 100.0f, 20.0f, 20000.0f},
                                {{127, Params{6.0f, 0.0f, 75.0f, 25.0f, 100.0f, 20.0f, 20000.0f}},
                                 {237, Params{0.0f, 1.0f, 100.0f, 0.0f, 0.0f, 1000.0f, 20000.0f}},
                                 {364, Params{0.0f, 5.0f, 100.0f, 0.0f, 200.0f, 20.0f, 5000.0f}}}};
        const auto route = run(type, {kQ - 1, 2, 17, 31, 7, 53}, automated, kQ, total);
        const auto automated_oracle = cpu_oracle({kQ - 1, 2, 17, 31, 7, 53}, automated, kQ, total);
        const ParamTimeline static_controls{automated.initial, {}};
        const auto static_oracle =
            cpu_oracle({kQ - 1, 2, 17, 31, 7, 53}, static_controls, kQ, total);
        require(max_abs_difference(automated_oracle, static_oracle) > 1.0e-3f,
                "automation schedule did not change the CPU oracle");
        compare_aligned(route, automated_oracle, 3 * kQ, "automated control CPU oracle");
        std::cout << "gpu_convolution_focused PASS "
                     "factory/native-rate/partition/PDC/in-place/automation/direct-gpu-delivery "
                     "left_gpu_blocks="
                  << direct_gpu_left << " right_gpu_blocks=" << direct_gpu_right << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gpu_convolution_focused FAIL: " << e.what() << "\n";
        return 1;
    }
}
