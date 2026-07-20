#pragma once

// A PluginSlot wrapper that crossfades the outgoing instance into the incoming one
// for a short window after a live swap, so replacing a hosted plugin instance is
// click-free (not just gap-free). It holds both instances + a TransitionMixer and
// blends old->new in process() via the shared signal::blend_fade_out primitive — the
// same crossfade law the reload hot-swap slot uses. Every non-process method delegates
// to the new (surviving) instance, so the graph executor renders it as an ordinary
// PluginSlot with no executor-side changes.
//
// Lifetime: the wrapper OWNS the retained old instance. Once fade_done() is true the
// blend is a no-op (output is the pure new render), and the control thread collapses
// the node back to the bare new slot + drops the wrapper (freeing the old instance on
// the control thread, after RCU readers have drained) — never on the audio thread.

#include <pulp/format/process_block.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/transition_mixer.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace pulp::host {

class CrossfadePluginSlot final : public PluginSlot {
public:
    // fade_samples <= 0 or a null old instance means "no fade" — the wrapper still
    // renders the new instance but blend is skipped (done from the start). out_channels
    // / max_block size the scratch that the old render targets before the blend.
    CrossfadePluginSlot(std::shared_ptr<PluginSlot> new_slot,
                        std::shared_ptr<PluginSlot> old_slot,
                        std::size_t fade_samples,
                        signal::TransitionCurve curve,
                        int out_channels,
                        int max_block)
        : new_slot_(std::move(new_slot)),
          old_slot_(std::move(old_slot)),
          scratch_channels_(std::max(0, out_channels)),
          scratch_frames_(std::max(0, max_block)) {
        mixer_.configure(fade_samples, curve);
        if (fade_samples == 0 || !old_slot_) {
            done_.store(true, std::memory_order_release);
        }
        scratch_storage_.assign(
            static_cast<std::size_t>(scratch_channels_) * static_cast<std::size_t>(scratch_frames_),
            0.0f);
        scratch_ptrs_.resize(static_cast<std::size_t>(scratch_channels_));
        for (int c = 0; c < scratch_channels_; ++c) {
            scratch_ptrs_[static_cast<std::size_t>(c)] =
                scratch_storage_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(scratch_frames_);
        }
        // Pre-size the fading-out instance's MIDI scratch and cap it to reserved
        // capacity, so a fade-out slot that emits MIDI (arp, MIDI-FX, note-expression)
        // never heap-allocates on the audio thread — mirrors the reload hot-swap slot.
        old_midi_in_.reserve(64, 0);
        old_midi_out_.reserve(64, 0);
        old_midi_in_.set_realtime_capacity_limit(true);
        old_midi_out_.set_realtime_capacity_limit(true);
    }

    // True once the crossfade has fully completed (output is the pure new render).
    // Read on the control thread to collapse the wrapper back to the bare new slot.
    bool fade_done() const noexcept { return done_.load(std::memory_order_acquire); }

    // The surviving instance, for the control thread to reinstate once the fade is done.
    const std::shared_ptr<PluginSlot>& new_slot() const noexcept { return new_slot_; }

    // ── process: blend old -> new ────────────────────────────────────────────
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events,
                 int num_samples) override {
        new_slot_->process(output, input, midi_in, midi_out, param_events, num_samples);
        blend_old_into_(output, input, num_samples);
    }

    void process(format::ProcessBuffers& audio,
                 const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events,
                 int num_samples) override {
        if (skip_blend_(audio, num_samples)) {
            new_slot_->process(audio, midi_in, midi_out, param_events, num_samples);
            return;
        }
        // Render OLD into the main output, snapshot it to scratch, then render NEW over
        // the top and blend scratch(old) -> output(new). Rendering both through the real
        // ProcessBuffers keeps the surviving instance's bus routing intact.
        old_midi_in_.clear();
        old_midi_out_.clear();
        old_slot_->process(audio, old_midi_in_, old_midi_out_, empty_params_, num_samples);
        blend_after_old_(audio, [&] {
            new_slot_->process(audio, midi_in, midi_out, param_events, num_samples);
        }, num_samples);
    }

    void process(format::ProcessBuffers& audio,
                 const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events,
                 int num_samples,
                 const format::ProcessContext& transport) override {
        if (skip_blend_(audio, num_samples)) {
            new_slot_->process(audio, midi_in, midi_out, param_events, num_samples, transport);
            return;
        }
        // Both instances see transport: a transport-sensitive fading-out instance must
        // keep its playhead/tempo across the swap or it would step at fade start.
        old_midi_in_.clear();
        old_midi_out_.clear();
        old_slot_->process(audio, old_midi_in_, old_midi_out_, empty_params_, num_samples, transport);
        blend_after_old_(audio, [&] {
            new_slot_->process(audio, midi_in, midi_out, param_events, num_samples, transport);
        }, num_samples);
    }

    // ── delegate everything else to the surviving instance ───────────────────
    const PluginInfo& info() const override { return new_slot_->info(); }
    bool is_loaded() const override { return new_slot_->is_loaded(); }
    bool prepare(double sr, int block) override { return new_slot_->prepare(sr, block); }
    void release() override { new_slot_->release(); }
    std::vector<HostParamInfo> parameters() const override { return new_slot_->parameters(); }
    float get_parameter(uint32_t id) const override { return new_slot_->get_parameter(id); }
    void set_parameter(uint32_t id, float v) override { new_slot_->set_parameter(id, v); }
    void set_bypass(bool b) override { new_slot_->set_bypass(b); }
    bool is_bypassed() const override { return new_slot_->is_bypassed(); }
    std::vector<uint8_t> save_state() const override { return new_slot_->save_state(); }
    bool restore_state(const std::vector<uint8_t>& d) override { return new_slot_->restore_state(d); }
    bool has_editor() const override { return new_slot_->has_editor(); }
    void* create_editor_view() override { return new_slot_->create_editor_view(); }
    void destroy_editor_view() override { new_slot_->destroy_editor_view(); }
    std::unique_ptr<HostedEditor> create_hosted_editor(void* parent) override {
        return new_slot_->create_hosted_editor(parent);
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        new_slot_->destroy_hosted_editor(std::move(ed));
    }
    int latency_samples() const override { return new_slot_->latency_samples(); }
    LatencyQuery latency_query() const override { return new_slot_->latency_query(); }
    int tail_samples() const override { return new_slot_->tail_samples(); }
    void accept(NativeHandleVisitor& v) const override { new_slot_->accept(v); }
    bool wants_transport() const override { return new_slot_->wants_transport(); }

private:
    // True when there is nothing to blend for this ProcessBuffers block (fade already
    // done, no old instance, no main bus, or a block larger than the scratch — which
    // snaps the fade to complete). The caller then just renders the new instance.
    bool skip_blend_(format::ProcessBuffers& audio, int num_samples) {
        if (done_.load(std::memory_order_relaxed) || !old_slot_) return true;
        auto* main_out = audio.main_output();
        if (main_out == nullptr) return true;
        const int ch = std::min<int>(scratch_channels_, static_cast<int>(main_out->num_channels()));
        if (num_samples > scratch_frames_ || ch <= 0) {
            done_.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    // The old instance has already rendered into the main output; snapshot it to
    // scratch, render the new instance over the top, then blend scratch(old) into the
    // output(new) under the mixer. RT-safe: no alloc/lock. skip_blend_ has guaranteed a
    // main bus, ch>0, and num_samples <= scratch_frames_.
    template <class RenderNew>
    void blend_after_old_(format::ProcessBuffers& audio, RenderNew&& render_new, int num_samples) {
        auto* main_out = audio.main_output();
        const int ch = std::min<int>(scratch_channels_, static_cast<int>(main_out->num_channels()));
        for (int c = 0; c < ch; ++c) {
            std::copy_n(main_out->channel_ptr(static_cast<std::size_t>(c)),
                        static_cast<std::size_t>(num_samples),
                        scratch_ptrs_[static_cast<std::size_t>(c)]);
        }
        render_new();
        audio::BufferView<float> scratch(scratch_ptrs_.data(), static_cast<std::size_t>(ch),
                                         static_cast<std::size_t>(num_samples));
        if (signal::blend_fade_out(mixer_, *main_out, scratch)) {
            done_.store(true, std::memory_order_release);
        }
    }

    // Render the fading-out instance into scratch (same input, no MIDI/param events —
    // it is on its way out) and blend it into `output` under the mixer. RT-safe: no
    // alloc/lock (scratch + mixer are pre-sized at construction). Used by the
    // transport-less BufferView overload.
    void blend_old_into_(audio::BufferView<float>& output,
                         const audio::BufferView<const float>& input,
                         int num_samples) {
        if (done_.load(std::memory_order_relaxed) || !old_slot_) return;
        const int ch = std::min<int>(scratch_channels_, static_cast<int>(output.num_channels()));
        if (num_samples > scratch_frames_ || ch <= 0) {
            // Block larger than the scratch (misdeclared block size): snap to full-new.
            done_.store(true, std::memory_order_release);
            return;
        }
        audio::BufferView<float> scratch(scratch_ptrs_.data(), static_cast<std::size_t>(ch),
                                         static_cast<std::size_t>(num_samples));
        old_midi_in_.clear();
        old_midi_out_.clear();
        old_slot_->process(scratch, input, old_midi_in_, old_midi_out_, empty_params_, num_samples);
        if (signal::blend_fade_out(mixer_, output, scratch)) {
            done_.store(true, std::memory_order_release);
        }
    }

    std::shared_ptr<PluginSlot> new_slot_;
    std::shared_ptr<PluginSlot> old_slot_;
    signal::TransitionMixer mixer_;
    std::vector<float> scratch_storage_;
    std::vector<float*> scratch_ptrs_;
    int scratch_channels_ = 0;
    int scratch_frames_ = 0;
    std::atomic<bool> done_{false};
    midi::MidiBuffer old_midi_in_;
    midi::MidiBuffer old_midi_out_;
    ParameterEventQueue empty_params_;
};

}  // namespace pulp::host
