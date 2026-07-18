#include "timeline_example_engine.hpp"

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace pulp::examples::timeline_phase1 {
namespace {

class InlineCompileExecutor final : public playback::CompileExecutor {
  public:
    bool submit(std::unique_ptr<playback::CompileTask> task,
                std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        constexpr std::size_t kWorkPerSlice = 4096;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                kWorkPerSlice}) == playback::CompileTaskStatus::Pending) {
        }
        return true;
    }
};

class ExampleSineSynthSlot final : public host::PluginSlot {
  public:
    ExampleSineSynthSlot() {
        info_.name = "Timeline example sine destination";
        info_.format = host::PluginFormat::CLAP;
        info_.num_inputs = 0;
        info_.num_outputs = 2;
        info_.category = "Instrument";
    }

    const host::PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double sample_rate, int) override {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48'000.0;
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        for (std::size_t note = 0; note < increments_.size(); ++note) {
            const double frequency = 440.0 * std::exp2((static_cast<double>(note) - 69.0) / 12.0);
            increments_[note] = kTwoPi * frequency / sample_rate_;
        }
        reset_voices();
        return true;
    }
    void release() override { reset_voices(); }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const host::ParameterEventQueue&, int num_samples) override {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::fill_n(output.channel_ptr(channel), static_cast<std::size_t>(num_samples), 0.0f);

        auto event = midi_in.begin();
        const auto end = midi_in.end();
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        for (int frame = 0; frame < num_samples; ++frame) {
            while (event != end && event->sample_offset <= frame) {
                const auto note = event->note();
                if (event->is_note_on() && event->velocity() != 0) {
                    voices_[note].active = true;
                    voices_[note].level = static_cast<float>(event->velocity()) / 127.0f;
                } else if (event->is_note_off() ||
                           (event->is_note_on() && event->velocity() == 0)) {
                    voices_[note].active = false;
                    voices_[note].level = 0.0f;
                }
                ++event;
            }

            float mixed = 0.0f;
            for (std::size_t note = 0; note < voices_.size(); ++note) {
                auto& voice = voices_[note];
                if (!voice.active)
                    continue;
                mixed += static_cast<float>(std::sin(voice.phase)) * voice.level * 0.12f;
                voice.phase += increments_[note];
                if (voice.phase >= kTwoPi)
                    voice.phase -= kTwoPi;
            }
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
                output.channel_ptr(channel)[frame] = mixed;
        }
    }

    bool has_active_notes() const noexcept {
        return std::any_of(voices_.begin(), voices_.end(),
                           [](const Voice& voice) { return voice.active; });
    }

    std::vector<host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

  private:
    struct Voice {
        double phase = 0.0;
        float level = 0.0f;
        bool active = false;
    };
    void reset_voices() noexcept {
        for (auto& voice : voices_)
            voice = {};
    }

    host::PluginInfo info_;
    std::array<Voice, 128> voices_{};
    std::array<double, 128> increments_{};
    double sample_rate_ = 48'000.0;
};

} // namespace

struct TimelineExampleEngine::Impl {
    playback::PlaybackProgramStore store;
    InlineCompileExecutor executor;
    playback::PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};
    host::SignalGraph graph;
    host::TimelineGraphPlaybackBinding binding{graph, store};
    playback::MasterTransport transport;
    playback::TransportSnapshot last_transport;
    host::NodeId synth_node = 0;
    std::uint32_t maximum_block_size = 0;
    bool ready = false;

    bool setup(playback::ProgramCompileRequest request, double sample_rate,
               std::uint32_t max_block_size, bool add_audible_synth) {
        if (!request.project || !request.tempo_map || max_block_size == 0 || sample_rate <= 0.0)
            return false;
        maximum_block_size = max_block_size;
        request.dirty.all = true;
        if (!compiler.submit(std::move(request)) || compiler.status().has_error)
            return false;
        auto program = store.read();
        if (!program || program->tracks().size() != 1)
            return false;

        graph.set_parallel_routing_enabled(false);
        const auto output_node = graph.add_output_node(2, "Timeline output");
        if (output_node == 0)
            return false;

        if (add_audible_synth) {
            synth_node = graph.add_plugin_node(std::make_unique<ExampleSineSynthSlot>(), 0, 2,
                                               "Audible note destination");
            if (synth_node == 0 || !graph.connect(synth_node, 0, output_node, 0) ||
                !graph.connect(synth_node, 1, output_node, 1))
                return false;
        }

        const std::array routes{host::TimelineTrackGraphRoute{
            program->tracks().front()->id(), output_node, 0, synth_node}};
        host::TimelineGraphBindingConfig config;
        config.audio_channels = 2;
        config.maximum_note_events_per_track_per_block = 256;
        config.audio_limits.max_channels = 2;
        config.audio_limits.max_block_frames = max_block_size;
        if (!binding.prepare(*program, routes, config, sample_rate,
                             static_cast<int>(max_block_size)))
            return false;

        playback::MasterTransportConfig transport_config;
        transport_config.max_buffer_size = max_block_size;
        transport_config.initially_playing = true;
        if (transport.prepare(program->tempo_map(), transport_config) != playback::TransportError::None)
            return false;
        last_transport.tempo_map = &program->tempo_map();
        last_transport.sample_rate = program->tempo_map().sample_rate();
        ready = true;
        return true;
    }

    bool recompile(playback::ProgramCompileRequest request) {
        if (!ready || !request.project)
            return false;
        auto live = store.read();
        if (!live)
            return false;
        request.tempo_map = live->tempo_map_owner();
        request.dirty.all = true;
        return compiler.submit(std::move(request)) && !compiler.status().has_error;
    }
};

TimelineExampleEngine::TimelineExampleEngine() = default;
TimelineExampleEngine::~TimelineExampleEngine() = default;

bool TimelineExampleEngine::prepare(playback::ProgramCompileRequest request, double sample_rate,
                                    std::uint32_t maximum_block_size,
                                    bool add_audible_synth) {
    auto replacement = std::make_unique<Impl>();
    if (!replacement->setup(std::move(request), sample_rate, maximum_block_size,
                            add_audible_synth))
        return false;
    impl_ = std::move(replacement);
    return true;
}

bool TimelineExampleEngine::recompile(playback::ProgramCompileRequest request) {
    return impl_ && impl_->recompile(std::move(request));
}

host::TimelineGraphProcessResult
TimelineExampleEngine::process(audio::BufferView<float>& output,
                               const audio::BufferView<const float>& input) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (!impl_ || !impl_->ready || output.num_samples() > impl_->maximum_block_size) {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::fill(output.channel(channel).begin(), output.channel(channel).end(), 0.0f);
        return {host::TimelineGraphProcessCode::CapacityExceeded};
    }
    if (impl_->transport.begin_block(static_cast<std::uint32_t>(output.num_samples()),
                                     impl_->last_transport) != playback::TransportError::None) {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::fill(output.channel(channel).begin(), output.channel(channel).end(), 0.0f);
        return {host::TimelineGraphProcessCode::InvalidTransport};
    }
    return impl_->binding.process(output, input, impl_->last_transport);
}

playback::TransportError TimelineExampleEngine::set_playing(bool playing) noexcept {
    return impl_ ? impl_->transport.set_playing(playing) : playback::TransportError::NotPrepared;
}

playback::TransportError TimelineExampleEngine::seek_samples(std::int64_t sample) noexcept {
    if (!impl_ || !impl_->last_transport.tempo_map)
        return playback::TransportError::NotPrepared;
    return impl_->transport.seek(impl_->last_transport.tempo_map->samples_to_ticks({sample}));
}

playback::TransportError TimelineExampleEngine::set_loop_samples(bool enabled, std::int64_t start,
                                                                 std::int64_t end) noexcept {
    if (!impl_ || !impl_->last_transport.tempo_map)
        return playback::TransportError::NotPrepared;
    const auto& map = *impl_->last_transport.tempo_map;
    return impl_->transport.set_loop(
        {enabled, map.samples_to_ticks({start}), map.samples_to_ticks({end})});
}

bool TimelineExampleEngine::prepared() const noexcept { return impl_ && impl_->ready; }

bool TimelineExampleEngine::synth_has_active_notes() const noexcept {
    if (!impl_ || impl_->synth_node == 0)
        return false;
    const auto* slot = impl_->graph.live_plugin_slot(impl_->synth_node);
    const auto* synth = dynamic_cast<const ExampleSineSynthSlot*>(slot);
    return synth && synth->has_active_notes();
}

const playback::TransportSnapshot& TimelineExampleEngine::last_transport() const noexcept {
    static const playback::TransportSnapshot empty;
    return impl_ ? impl_->last_transport : empty;
}

} // namespace pulp::examples::timeline_phase1
