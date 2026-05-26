#pragma once

// PulpHostBench — the DAW-bench validation plugin.
//
// Purpose: load this plugin in every DAW under test, drive it through the
// per-DAW manual script at `docs/validation/daw-bench/`, and read back the
// log file at `~/Library/Logs/PulpHostBench/<host>-<format>-<ts>.log` to
// confirm which quirk rows in
// `planning/2026-05-24-daw-host-quirks-inheritance.md` the host actually
// triggered. The aggregator at `tools/scripts/promote_quirk_tiers.py`
// reads those logs to produce a patch promoting `Speculative` →
// `Validated` rows in `core/format/include/pulp/format/host_quirks.hpp`.
//
// The plugin is intentionally minimal as a DSP — a passthrough with a
// gain knob. The interesting surface is the **lifecycle events** every
// override records into the bench log.

#include "bench_logger.hpp"

#include <pulp/format/processor.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace pulp::examples {

enum HostBenchParams : state::ParamID {
    kBenchGain    = 1,  ///< User-driven param (host writes this; we log)
    kBenchBypass  = 2,  ///< User-driven bypass (host may also synthesize one)
    kBenchTrigger = 3,  ///< User-driven button; the per-DAW script asks the
                         ///< human to wiggle this to confirm gestures work
};

/// Hard-coded version. Bumping this should also bump the row in
/// `planning/host-quirks-log.md`'s "tooling versions" section so the
/// aggregator can refuse logs from older mismatched plugin builds.
inline constexpr const char* kBenchPluginVersion = "1.0.0";

class HostBenchProcessor : public format::Processor {
public:
    explicit HostBenchProcessor(std::string format_label)
        : format_label_(std::move(format_label)),
          host_type_(format::detect_host_type()),
          logger_(std::make_unique<bench::BenchLogger>(format_label_, host_type_)) {
        logger_->write_event("processor_construct",
                             {{"plugin_version", kBenchPluginVersion}});
    }

    ~HostBenchProcessor() override {
        if (logger_) {
            logger_->write_event("processor_destruct", {});
        }
    }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpHostBench",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.host-bench",
            .version = kBenchPluginVersion,
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}, {"Sidechain", 2, /*optional=*/true}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kBenchGain,
            .name = "Bench Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kBenchBypass,
            .name = "Bench Bypass",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
        store.add_parameter({
            .id = kBenchTrigger,
            .name = "Bench Trigger",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
        logger_->write_event("define_parameters",
                             {{"count", bench::BenchLogger::to_str(store.param_count())}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        prepared_ = true;
        sample_rate_.store(static_cast<int>(ctx.sample_rate), std::memory_order_relaxed);
        max_buffer_.store(ctx.max_buffer_size, std::memory_order_relaxed);
        prepare_count_.fetch_add(1, std::memory_order_relaxed);
        logger_->write_event("prepare",
                             {{"sample_rate", bench::BenchLogger::to_str(ctx.sample_rate)},
                              {"max_buffer_size", bench::BenchLogger::to_str(ctx.max_buffer_size)},
                              {"input_channels", bench::BenchLogger::to_str(ctx.input_channels)},
                              {"output_channels", bench::BenchLogger::to_str(ctx.output_channels)},
                              {"prepare_count", bench::BenchLogger::to_str(prepare_count_.load())}});
    }

    void release() override {
        prepared_ = false;
        logger_->write_event("release", {});
    }

    void suspend() override {
        logger_->write_event("suspend", {});
    }

    void resume() override {
        logger_->write_event("resume", {});
    }

    int latency_samples() const override { return 0; }

    bool is_bus_layout_supported(const BusesLayout& layout) const override {
        std::string in_csv, out_csv;
        for (size_t i = 0; i < layout.inputs.size(); ++i) {
            if (i) in_csv += ',';
            in_csv += bench::BenchLogger::to_str(layout.inputs[i]);
        }
        for (size_t i = 0; i < layout.outputs.size(); ++i) {
            if (i) out_csv += ',';
            out_csv += bench::BenchLogger::to_str(layout.outputs[i]);
        }
        logger_->write_event("bus_layout_proposal",
                             {{"inputs", in_csv}, {"outputs", out_csv}});
        // Bench plugin is permissive: accept any mono/stereo combo, plus
        // record the proposal regardless. The default Processor policy is
        // stricter than what some hosts (Reaper R3, FL Studio) send.
        auto ok = [](int n) { return n >= 1 && n <= 8; };
        for (int n : layout.inputs)  if (!ok(n)) return false;
        for (int n : layout.outputs) if (!ok(n)) return false;
        return true;
    }

    void on_host_transport_changed(bool is_playing, double position_seconds) override {
        logger_->write_event("transport_changed",
                             {{"is_playing", bench::BenchLogger::bool_str(is_playing)},
                              {"position_seconds", bench::BenchLogger::to_str(position_seconds)}});
    }

    void on_host_tempo_changed(double new_tempo_bpm) override {
        logger_->write_event("tempo_changed",
                             {{"tempo_bpm", bench::BenchLogger::to_str(new_tempo_bpm)}});
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        logger_->write_event("serialize_plugin_state", {});
        // Magic marker so the host's blob round-trip can be validated.
        std::string marker = "PULP-HOST-BENCH-v1";
        return std::vector<uint8_t>(marker.begin(), marker.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        std::string marker(data.begin(), data.end());
        bool ok = (marker == "PULP-HOST-BENCH-v1");
        logger_->write_event("deserialize_plugin_state",
                             {{"bytes", bench::BenchLogger::to_str(data.size())},
                              {"marker_ok", bench::BenchLogger::bool_str(ok)}});
        return ok || data.empty();
    }

    void on_view_opened(view::View&) override {
        view_open_count_.fetch_add(1, std::memory_order_relaxed);
        logger_->write_event("view_opened",
                             {{"open_count", bench::BenchLogger::to_str(view_open_count_.load())}});
    }
    void on_view_closed(view::View&) override {
        logger_->write_event("view_closed", {});
    }
    void on_view_resized(view::View&, uint32_t w, uint32_t h) override {
        logger_->write_event("view_resized",
                             {{"width", bench::BenchLogger::to_str(w)},
                              {"height", bench::BenchLogger::to_str(h)}});
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const format::ProcessContext& ctx) override
    {
        process_block_count_.fetch_add(1, std::memory_order_relaxed);

        // Honest spec-violation canary: was the host able to call process()
        // even though we never got a prepare()? That's row #13 (FL Studio)
        // and a worth-knowing signal for any host.
        if (!prepared_) {
            unprepared_process_count_.fetch_add(1, std::memory_order_relaxed);
            logger_->write_event("process_without_prepare",
                                 {{"block_count", bench::BenchLogger::to_str(process_block_count_.load())}});
        }

        const int n = ctx.num_samples;
        const double sr = ctx.sample_rate;
        const int last_sr = sample_rate_.load(std::memory_order_relaxed);
        if (static_cast<int>(sr) != last_sr && last_sr != 0) {
            logger_->write_event("process_sample_rate_drift",
                                 {{"prepared_sr", bench::BenchLogger::to_str(last_sr)},
                                  {"process_sr", bench::BenchLogger::to_str(sr)}});
        }

        const int last_max = max_buffer_.load(std::memory_order_relaxed);
        if (n > last_max) {
            logger_->write_event("process_buffer_overrun",
                                 {{"prepared_max", bench::BenchLogger::to_str(last_max)},
                                  {"process_n", bench::BenchLogger::to_str(n)}});
        }

        if (ctx.is_playing != last_is_playing_) {
            last_is_playing_ = ctx.is_playing;
            logger_->write_event("process_is_playing_edge",
                                 {{"is_playing", bench::BenchLogger::bool_str(ctx.is_playing)}});
        }

        // Honor bypass; light DSP — bench-time correctness over speed.
        bool bypass = state().get_value(kBenchBypass) >= 0.5f;
        float gain_db = state().get_value(kBenchGain);
        float gain = bypass ? 1.0f : std::pow(10.0f, gain_db / 20.0f);

        const std::size_t channels = output.num_channels();
        const std::size_t in_channels = input.num_channels();
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto out = output.channel(ch);
            if (ch < in_channels) {
                auto in = input.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i) {
                    out[i] = in[i] * gain;
                }
            } else {
                for (std::size_t i = 0; i < output.num_samples(); ++i) {
                    out[i] = 0.0f;
                }
            }
        }

        // Sidechain observation — only logged on edges (connect/disconnect),
        // not every block, to keep logs manageable.
        bool sc_now = (sidechain_input() != nullptr);
        if (sc_now != last_sidechain_connected_) {
            last_sidechain_connected_ = sc_now;
            logger_->write_event("sidechain_edge",
                                 {{"connected", bench::BenchLogger::bool_str(sc_now)}});
        }

        // MIDI in count — edge-logged only.
        const auto midi_count = midi_in.size();
        if (midi_count > 0 && midi_count != last_midi_in_count_) {
            last_midi_in_count_ = midi_count;
            logger_->write_event("midi_in",
                                 {{"events", bench::BenchLogger::to_str(midi_count)}});
        }
        (void)midi_out;
    }

    // Helpers a custom view would call.
    std::string format_label() const { return format_label_; }
    format::HostType host_type() const { return host_type_; }
    std::string last_event() const { return logger_ ? logger_->last_event() : ""; }
    std::uint64_t event_count() const { return logger_ ? logger_->event_count() : 0; }
    std::string log_path() const { return logger_ ? logger_->path() : ""; }
    int prepare_count() const { return prepare_count_.load(std::memory_order_relaxed); }
    int process_block_count() const { return process_block_count_.load(std::memory_order_relaxed); }
    int unprepared_process_count() const {
        return unprepared_process_count_.load(std::memory_order_relaxed);
    }

private:
    std::string format_label_;
    format::HostType host_type_;
    std::unique_ptr<bench::BenchLogger> logger_;
    std::atomic<bool> prepared_{false};
    // double is not std::atomic_double-safe on every supported toolchain;
    // store as a relaxed integer in Hz (rounded). The drift check below
    // tolerates fractional rates by comparing rounded values.
    std::atomic<int> sample_rate_{0};
    std::atomic<int> max_buffer_{0};
    std::atomic<int> prepare_count_{0};
    std::atomic<int> process_block_count_{0};
    std::atomic<int> unprepared_process_count_{0};
    std::atomic<int> view_open_count_{0};
    bool last_is_playing_ = false;
    bool last_sidechain_connected_ = false;
    std::size_t last_midi_in_count_ = 0;
};

inline std::unique_ptr<format::Processor> create_host_bench_au() {
    return std::make_unique<HostBenchProcessor>("AU");
}
inline std::unique_ptr<format::Processor> create_host_bench_vst3() {
    return std::make_unique<HostBenchProcessor>("VST3");
}
inline std::unique_ptr<format::Processor> create_host_bench_clap() {
    return std::make_unique<HostBenchProcessor>("CLAP");
}
// Default factory used by pulp_add_plugin / registry. The format label
// will be reset by the format-specific entry TUs above when a real host
// loads the plugin; the registry path is used for ctest dlopen + the
// standalone host.
inline std::unique_ptr<format::Processor> create_host_bench() {
    return std::make_unique<HostBenchProcessor>("Standalone");
}

}  // namespace pulp::examples
