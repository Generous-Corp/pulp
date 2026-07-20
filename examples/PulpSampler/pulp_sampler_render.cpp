#include "pulp_sampler.hpp"

namespace pulp::examples {

namespace {

std::uint64_t tail_frames_consumed(std::size_t output_frames, std::size_t planned_input_frames,
                                   std::size_t source_frames_rendered) noexcept {
    if (planned_input_frames == 0)
        return output_frames;
    const auto valid = std::min(planned_input_frames, source_frames_rendered);
    const auto missing = planned_input_frames - valid;
    return static_cast<std::uint64_t>((static_cast<long double>(missing) * output_frames) /
                                      planned_input_frames);
}

std::uint32_t voice_activity_frames(std::uint32_t output_frames, std::size_t planned_input_frames,
                                    std::size_t source_frames_rendered,
                                    std::uint64_t available_tail_frames) noexcept {
    const auto tail_window = std::min<std::uint64_t>(
        output_frames,
        tail_frames_consumed(output_frames, planned_input_frames, source_frames_rendered));
    const auto source_backed = output_frames - tail_window;
    return static_cast<std::uint32_t>(source_backed + std::min(tail_window, available_tail_frames));
}

} // namespace

void PulpSamplerProcessor::process(audio::BufferView<float>& output,
                                   const audio::BufferView<const float>&, midi::MidiBuffer& midi_in,
                                   midi::MidiBuffer&, const format::ProcessContext&) {
    clear_output(output);
    if (!prepared_)
        return;

    const auto audio_generation = streaming_->begin_audio_callback();
    flush_pending_cancellations();

    const auto published = streaming_->published_source();
    const bool can_trigger = published_source_valid(published);

    const auto params = current_params();
    const auto block_frames = static_cast<std::uint32_t>(output.num_samples());
    midi_in.sort();

    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < midi_in.size(); ++i) {
        const auto& event = midi_in[i];
        const auto offset = static_cast<std::uint32_t>(
            std::clamp(event.sample_offset, 0, static_cast<int32_t>(block_frames)));
        if (offset > cursor) {
            render_output_segment(output, cursor, offset - cursor, params);
        }

        if (event.message.isNoteOn() && can_trigger) {
            trigger_note(event.message.getNoteNumber(),
                         static_cast<float>(event.message.getVelocity()) / 127.0f, published,
                         params);
        } else if (event.message.isNoteOff()) {
            release_note(event.message.getNoteNumber());
        }
        cursor = offset;
    }

    if (cursor < block_frames) {
        render_output_segment(output, cursor, block_frames - cursor, params);
    }

    source_publication_.acknowledge_audio(published, voices_, *streaming_);
    flush_pending_cancellations();
    streaming_->complete_audio_callback(audio_generation);
    publish_heritage_runtime_snapshot();
    publish_envelope_diagnostics();
    publish_interpolation_diagnostics();
}

bool PulpSamplerProcessor::published_source_valid(
    const SamplerPublishedSource& source) const noexcept {
    if (source.kind == SamplerPublishedSourceKind::Resident)
        return source_publication_.sample_store().slot_view_valid(source.resident);
    return source.kind == SamplerPublishedSourceKind::Streamed && source.streamed.valid();
}

PulpSamplerProcessor::RenderParams PulpSamplerProcessor::current_params() const {
    RenderParams params;
    const float gain_db = state().get_value(kSamplerGain);
    params.gain = std::pow(10.0f, gain_db / 20.0f);
    params.adsr.attack = state().get_value(kSamplerAttack) / 1000.0f;
    params.adsr.decay = state().get_value(kSamplerDecay) / 1000.0f;
    params.adsr.sustain = state().get_value(kSamplerSustain) / 100.0f;
    params.adsr.release = state().get_value(kSamplerRelease) / 1000.0f;
    params.pitch_semitones = state().get_value(kSamplerPitch);
    params.heritage_clock_multiplier =
        state().get_value(kSamplerHeritageClockRatio);
    params.loop = state().get_value(kSamplerLoop) >= 0.5f;
    params.reverse = state().get_value(kSamplerReverse) >= 0.5f;
    const auto interpolation =
        std::clamp(static_cast<int>(std::lround(state().get_value(kSamplerInterpolation))), 0, 5);
    params.interpolation = static_cast<audio::SampleInterpolationPolicy>(interpolation);
    return params;
}

void PulpSamplerProcessor::clear_output(audio::BufferView<float>& output) noexcept {
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
        std::fill_n(output.channel_ptr(ch), output.num_samples(), 0.0f);
    }
}

void PulpSamplerProcessor::clear_output_segment(audio::BufferView<float>& output,
                                                std::uint32_t start_frame,
                                                std::uint32_t frames) noexcept {
    for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
        std::fill_n(output.channel_ptr(channel) + start_frame, frames, 0.0f);
    }
}

PulpSamplerPrepareResult PulpSamplerProcessor::prepare_stream_domain() {
    const auto stream_sample_rate = host_sample_rate_;
    const auto heritage_input_capacity = heritage_.maximum_stream_input_frames();
    const auto stream_block_frames = heritage_input_capacity == 0
                                         ? max_block_frames_
                                         : static_cast<std::uint32_t>(heritage_input_capacity);
    return bind_stream_domain_checked({stream_sample_rate, stream_block_frames},
                                      config_.streaming_memory_budget_bytes);
}

audio::LoopRegion PulpSamplerProcessor::make_region(std::uint64_t frames, double sample_rate,
                                                    bool loop, bool reverse) const noexcept {
    audio::LoopRegion region;
    region.start_frame = 0;
    region.end_frame = frames;
    region.source_sample_rate = sample_rate;
    region.playback_mode =
        loop ? (reverse ? audio::LoopPlaybackMode::Reverse : audio::LoopPlaybackMode::Forward)
             : (reverse ? audio::LoopPlaybackMode::ReverseOnce : audio::LoopPlaybackMode::OneShot);
    region.reverse_entry = reverse;
    region.interpolation = audio::LoopInterpolationMode::Linear;
    region.crossfade_curve = audio::LoopCrossfadeCurve::Linear;
    if (loop && frames >= 32) {
        region.crossfade_frames = std::min<std::uint64_t>({64, frames / 8, frames / 2});
    }
    return region;
}

audio::LoopRegion PulpSamplerProcessor::make_region(const audio::PublishedSampleView& sample,
                                                    const RenderParams& params) const noexcept {
    return make_region(sample.num_frames, sample.sample_rate, params.loop, params.reverse);
}

double PulpSamplerProcessor::playback_speed(int note, const audio::PublishedSampleView& sample,
                                            const RenderParams& params) const noexcept {
    return key_map_.playback_rate_for_note(
        note, sample.sample_rate, static_cast<double>(host_sample_rate_), params.pitch_semitones);
}

double PulpSamplerProcessor::playback_speed(int note, double sample_rate,
                                            const RenderParams& params) const noexcept {
    return key_map_.playback_rate_for_note(
        note, sample_rate, static_cast<double>(host_sample_rate_), params.pitch_semitones);
}

bool PulpSamplerProcessor::polynomial_mip_policy(audio::SampleInterpolationPolicy policy) noexcept {
    return policy == audio::SampleInterpolationPolicy::CubicHermite ||
           policy == audio::SampleInterpolationPolicy::CubicLagrange;
}

SamplerMipLevelView PulpSamplerProcessor::select_resident_mip(
    const SamplerPublishedSource& source, audio::SampleInterpolationPolicy policy,
    double base_source_frames_per_output, bool loop, bool reverse) noexcept {
    // Stored forward one-shot mips use clamped FIR boundaries. Loops and
    // reverse entries stay on the base asset so wrapped edges and the final
    // source-frame phase remain exact.
    if (!polynomial_mip_policy(policy) || loop || reverse)
        return {};
    const auto octave = sampler_exact_mip_octave(base_source_frames_per_output);
    const auto* level = source.resident_mips.level(octave);
    if (level != nullptr && heritage_.voice_artifact_path_active()) {
        resident_mip_suppressions_ = saturated_add(resident_mip_suppressions_, 1);
        return {};
    }
    return level == nullptr ? SamplerMipLevelView{} : *level;
}

const SamplerStreamMipLevelView* PulpSamplerProcessor::select_streamed_mip(
    const SamplerPublishedSource& source, audio::SampleInterpolationPolicy policy,
    double base_source_frames_per_output, bool loop, bool reverse) noexcept {
    if (!polynomial_mip_policy(policy) || loop || reverse)
        return nullptr;
    const auto* level =
        source.streamed_mips.level(sampler_exact_mip_octave(base_source_frames_per_output));
    if (level != nullptr && heritage_.voice_artifact_path_active()) {
        streamed_mip_suppressions_ = saturated_add(streamed_mip_suppressions_, 1);
        return nullptr;
    }
    return level;
}

audio::PreparedSampleInterpolation
PulpSamplerProcessor::prepared_interpolation(audio::SampleInterpolationPolicy policy,
                                             double source_frames_per_output) noexcept {
    audio::PreparedSampleInterpolation interpolation{.policy = policy};
    if (policy == audio::SampleInterpolationPolicy::RatioTrackingSinc) {
        interpolation.sinc = sinc_bank_->view().select(source_frames_per_output);
        if (!interpolation.sinc.valid()) {
            sinc_fallback_selections_ = saturated_add(sinc_fallback_selections_, 1);
            interpolation = {.policy = audio::SampleInterpolationPolicy::CubicHermite};
        }
    }
    return interpolation;
}

audio::PreparedSampleInterpolation
PulpSamplerProcessor::prepared_rate_safe_interpolation(audio::SampleInterpolationPolicy policy,
                                                       double source_frames_per_output) noexcept {
    if (polynomial_mip_policy(policy) && source_frames_per_output > 1.0) {
        if (heritage_.voice_artifact_path_active()) {
            sinc_promotion_suppressions_ = saturated_add(sinc_promotion_suppressions_, 1);
        } else {
            policy = audio::SampleInterpolationPolicy::RatioTrackingSinc;
        }
    }
    return prepared_interpolation(policy, source_frames_per_output);
}

audio::PreparedSampleInterpolation PulpSamplerProcessor::prepared_pitch_source_interpolation(
    audio::SampleInterpolationPolicy policy, double source_frames_per_output,
    double artifact_source_frames_per_output) noexcept {
    if (heritage_.voice_artifact_path_active() && polynomial_mip_policy(policy) &&
        artifact_source_frames_per_output > 1.0 && source_frames_per_output <= 1.0) {
        sinc_promotion_suppressions_ = saturated_add(sinc_promotion_suppressions_, 1);
    }
    return prepared_rate_safe_interpolation(policy, source_frames_per_output);
}

PulpSamplerProcessor::StreamRateContract PulpSamplerProcessor::stream_rate_contract(
    const audio::SampleAssetView& candidate, double candidate_source_frames_per_output,
    const RenderParams& params, std::size_t replaced_voice) const noexcept {
    // Asset/preload validation remains owned by the voice reader so its
    // distinct InvalidPreloadContract diagnostic is not masked as a rate
    // rejection.
    if (!candidate.has_stream_source || candidate.stream_source.page_frames == 0 ||
        !(candidate_source_frames_per_output > 0.0)) {
        return StreamRateContract::LegacyRejected;
    }
    const auto requested_clock_multiplier = heritage_.clock_automation_active()
        ? params.heritage_clock_multiplier
        : 1.0;
    double legacy_frames_per_second = candidate_source_frames_per_output * host_sample_rate_;
    double effective_frames_per_second = 0.0;
    const auto add_effective = [&](double pitch_ratio, double clock_multiplier,
                                   double source_sample_rate,
                                   double live_ratio) noexcept {
        const auto evaluation = audio::evaluate_sample_stream_consumption(
            {.pitch_ratio = pitch_ratio,
             .clock_ratio = 1.0,
             .live_source_consumption_ratio = live_ratio},
            SamplerStreamingRuntime::maximum_pitch_ratio());
        if (evaluation.status == audio::SampleStreamConsumptionStatus::LegacyRejected)
            return StreamRateContract::LegacyRejected;
        if (evaluation.status == audio::SampleStreamConsumptionStatus::HeritageRejected)
            return StreamRateContract::HeritageRejected;
        const auto contribution = source_sample_rate * evaluation.effective_ratio;
        if (!std::isfinite(contribution) ||
            !std::isfinite(effective_frames_per_second + contribution))
            return StreamRateContract::HeritageRejected;
        effective_frames_per_second += contribution;
        return StreamRateContract::Allowed;
    };
    const auto candidate_pitch_ratio = candidate_source_frames_per_output * host_sample_rate_ /
                                       static_cast<double>(candidate.sample_rate);
    auto factor_contract = add_effective(candidate_pitch_ratio,
                                         requested_clock_multiplier,
                                         candidate.sample_rate,
                                         heritage_.active_live_source_consumption_ratio());
    if (factor_contract != StreamRateContract::Allowed)
        return factor_contract;
    for (std::size_t index = 0; index < kMaxVoices; ++index) {
        if (index == replaced_voice)
            continue;
        const auto& voice = voices_[index];
        if (!voice.active || !voice.streamed)
            continue;
        // Decode workers serialize every source assigned to them. The asset view
        // does not expose that assignment, so aggregate all streamed voices to
        // keep each worker within one certified throughput budget.
        if (voice.stream_contract_fade_pending) {
            legacy_frames_per_second +=
                voice.heritage_pitch_factor * voice.streamed_asset.sample_rate;
            factor_contract =
                add_effective(voice.heritage_pitch_factor,
                              voice.heritage_clock_multiplier,
                              voice.streamed_asset.sample_rate,
                              voice.heritage_live_consumption_factor);
        } else {
            const auto ratio = key_map_.pitch_ratio_for_note(voice.note, params.pitch_semitones);
            legacy_frames_per_second += ratio * voice.streamed_asset.sample_rate;
            factor_contract = add_effective(ratio,
                                            requested_clock_multiplier,
                                            voice.streamed_asset.sample_rate,
                                            heritage_.active_live_source_consumption_ratio());
        }
        if (factor_contract != StreamRateContract::Allowed)
            return factor_contract;
    }
    auto certified_frames_per_second = static_cast<double>(candidate.stream_source.page_frames) /
                                       SamplerStreamingRuntime::certified_decoder_latency_seconds();
#if defined(PULP_SAMPLER_TEST_HOOKS)
    if (stream_rate_capacity_override_for_test_ > 0.0)
        certified_frames_per_second = stream_rate_capacity_override_for_test_;
#endif
    if (!std::isfinite(legacy_frames_per_second))
        return StreamRateContract::LegacyRejected;
    if (std::isfinite(effective_frames_per_second) &&
        effective_frames_per_second <= certified_frames_per_second) {
        return StreamRateContract::Allowed;
    }
    return legacy_frames_per_second <= certified_frames_per_second
               ? StreamRateContract::HeritageRejected
               : StreamRateContract::LegacyRejected;
}

PulpSamplerProcessor::StreamRateContract
PulpSamplerProcessor::pitch_rate_contract(double pitch_ratio,
                                          double clock_multiplier) const noexcept {
    if (!heritage_.clock_automation_active()) clock_multiplier = 1.0;
    const auto evaluation = audio::evaluate_sample_stream_consumption(
        {.pitch_ratio = pitch_ratio,
         .clock_ratio = 1.0,
         .live_source_consumption_ratio = heritage_.active_live_source_consumption_ratio()},
        SamplerStreamingRuntime::maximum_pitch_ratio());
    switch (evaluation.status) {
    case audio::SampleStreamConsumptionStatus::Allowed:
        return heritage_.runtime_rate_factors_supported(pitch_ratio,
                                                        clock_multiplier)
                   ? StreamRateContract::Allowed
                   : StreamRateContract::HeritageRejected;
    case audio::SampleStreamConsumptionStatus::HeritageRejected:
        return StreamRateContract::HeritageRejected;
    case audio::SampleStreamConsumptionStatus::LegacyRejected:
        return StreamRateContract::LegacyRejected;
    }
    return StreamRateContract::LegacyRejected;
}

audio::SampleStreamConsumptionDeclaration
PulpSamplerProcessor::stream_consumption_declaration(const SamplerVoice& voice) const noexcept {
    const auto evaluation = audio::evaluate_sample_stream_consumption(
        {.pitch_ratio = voice.heritage_pitch_factor,
         .clock_ratio = 1.0,
         .live_source_consumption_ratio = voice.heritage_live_consumption_factor},
        SamplerStreamingRuntime::maximum_pitch_ratio());
    if (!evaluation.allowed())
        return {};
    return {.source_frames_per_second =
                static_cast<double>(voice.streamed_asset.sample_rate) * evaluation.effective_ratio};
}

void PulpSamplerProcessor::render_output_segment(audio::BufferView<float>& output,
                                                 std::uint32_t start_frame, std::uint32_t frames,
                                                 const RenderParams& params) noexcept {
    if (frames == 0)
        return;
    if (heritage_.direct_path_allowed()) {
        render_active_voices(output, start_frame, frames, params);
        return;
    }
    if (!heritage_.processing_required()) {
        clear_output_segment(output, start_frame, frames);
        return;
    }
    if (output.num_channels() > kMaxOutputChannels) {
        heritage_.reject_process();
        clear_output_segment(output, start_frame, frames);
        return;
    }

    if (heritage_.typed_profile()) {
        std::fill_n(bus_voice_activity_.begin() + start_frame, frames, std::uint8_t{0});
        render_active_voices(output, start_frame, frames, params);
        if (!heritage_.processing_required()) {
            clear_output_segment(output, start_frame, frames);
            return;
        }
        if (!heritage_.bus_processing_required())
            return;

        std::array<float*, kMaxOutputChannels> segment_ptrs{};
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            segment_ptrs[channel] = output.channel_ptr(channel) + start_frame;
        audio::BufferView<float> segment(segment_ptrs.data(), output.num_channels(), frames);
        const auto activity =
            std::span<const std::uint8_t>(bus_voice_activity_.data() + start_frame, frames);
        if (!heritage_.process_bus(segment, activity))
            clear_output_segment(output, start_frame, frames);
        return;
    }

    audio::SampleHeritageProcessPlan plan;
    if (!heritage_.plan(frames, plan)) {
        clear_output_segment(output, start_frame, frames);
        return;
    }

    auto dry = heritage_.dry_buffer(plan.input_frames);
    clear_output(dry);
    render_active_voices(dry, 0, static_cast<std::uint32_t>(plan.input_frames), params);

    std::array<float*, kMaxOutputChannels> segment_ptrs{};
    for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
        segment_ptrs[channel] = output.channel_ptr(channel) + start_frame;
    audio::BufferView<float> segment(segment_ptrs.data(), output.num_channels(), frames);
    if (!heritage_.process(plan, segment))
        clear_output_segment(output, start_frame, frames);
}

void PulpSamplerProcessor::render_active_voices(audio::BufferView<float>& output,
                                                std::uint32_t start_frame, std::uint32_t frames,
                                                const RenderParams& params) noexcept {
    if (frames == 0)
        return;

    const auto output_channels =
        std::min<std::uint32_t>({static_cast<std::uint32_t>(output.num_channels()),
                                 prepared_output_channels_, kMaxOutputChannels});
    if (output_channels == 0)
        return;

    for (std::uint32_t ch = 0; ch < kMaxOutputChannels; ++ch) {
        voice_scratch_ptrs_[ch] = voice_scratch_[ch].data();
    }

    std::uint32_t rendered = 0;
    while (rendered < frames) {
        const auto chunk = std::min(frames - rendered, max_block_frames_);
        for (std::size_t voice_index = 0; voice_index < kMaxVoices; ++voice_index) {
            auto& voice = voices_[voice_index];
            if (!voice.active)
                continue;

            if (voice.streamed) {
                render_streamed_voice(voice_index, voice, output, start_frame + rendered, chunk,
                                      output_channels, params);
                continue;
            }

            const bool process_heritage_voice = heritage_.voice_processing_required();
            const bool draining_heritage_tail = voice.heritage_source_exhausted;
            const auto voice_frames = draining_heritage_tail
                                          ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                                chunk, voice.heritage_tail_frames_remaining))
                                          : chunk;
            audio::BufferView<float> voice_scratch(voice_scratch_ptrs_.data(), output_channels,
                                                   voice_frames);
            SamplerHeritageRuntime::VoiceProcessPlan heritage_plan;
            std::uint32_t raw_frames = voice_frames;
            std::uint64_t rendered_raw_frames = raw_frames;
            audio::BufferView<float> raw = voice_scratch;
            const auto pitch_factor =
                draining_heritage_tail
                    ? voice.heritage_pitch_factor
                    : key_map_.pitch_ratio_for_note(voice.note, params.pitch_semitones);
            auto effective_pitch_factor = pitch_factor;
            auto effective_clock_multiplier = draining_heritage_tail
                ? voice.heritage_clock_multiplier
                : (heritage_.clock_automation_active()
                       ? params.heritage_clock_multiplier
                       : 1.0);
            if (!draining_heritage_tail &&
                !heritage_.runtime_rate_factors_supported(
                    effective_pitch_factor, effective_clock_multiplier)) {
                effective_pitch_factor = voice.heritage_pitch_factor;
                effective_clock_multiplier = voice.heritage_clock_multiplier;
                if (!voice.heritage_rate_automation_rejected) {
                    heritage_.record_rate_automation_rejection();
                    voice.heritage_rate_automation_rejected = true;
                }
            } else if (!draining_heritage_tail) {
                voice.heritage_pitch_factor = effective_pitch_factor;
                voice.heritage_clock_multiplier = effective_clock_multiplier;
                voice.heritage_rate_automation_rejected = false;
            }
            if (process_heritage_voice) {
                const auto planned = draining_heritage_tail
                                         ? heritage_.plan_voice_tail(voice_index, voice_frames,
                                                                     effective_pitch_factor,
                                                                     effective_clock_multiplier,
                                                                     heritage_plan)
                                         : heritage_.plan_voice(voice_index, voice_frames,
                                                                effective_pitch_factor,
                                                                effective_clock_multiplier,
                                                                heritage_plan);
                if (!planned) {
                    reset_voice(voice);
                    continue;
                }
                raw_frames = static_cast<std::uint32_t>(heritage_plan.input_frames);
                raw = heritage_.voice_dry_buffer(voice_index, raw_frames);
                clear_output(raw);
            }

            bool source_active = !draining_heritage_tail;
            if (source_active) {
                std::array<const float*, kMaxSampleChannels> sample_ptrs{};
                std::uint64_t source_frames = voice.sample.num_frames;
                double source_sample_rate = voice.sample.sample_rate;
                if (voice.resident_mip.valid()) {
                    sample_ptrs = voice.resident_mip.channels;
                    source_frames = voice.resident_mip.frames;
                    source_sample_rate = voice.resident_mip.sample_rate;
                } else if (!source_publication_.sample_store().populate_channel_ptrs(
                               voice.sample, sample_ptrs.data(), sample_ptrs.size())) {
                    reset_voice(voice);
                    continue;
                }
                audio::BufferView<const float> source(sample_ptrs.data(), voice.sample.num_channels,
                                                      static_cast<std::size_t>(source_frames));

                const auto source_frames_per_output =
                    heritage_.pitch_active()
                        ? source_sample_rate / static_cast<double>(host_sample_rate_)
                        : effective_pitch_factor * source_sample_rate /
                              static_cast<double>(host_sample_rate_);
                const auto interpolation = prepared_pitch_source_interpolation(
                    params.interpolation, source_frames_per_output,
                    effective_pitch_factor * source_sample_rate /
                        static_cast<double>(host_sample_rate_));
                if (!(source_frames_per_output > 0.0) || !interpolation.valid() ||
                    !voice.renderer.set_interpolation(interpolation)) {
                    reset_voice(voice);
                    continue;
                }
                voice.renderer.set_playback_rate(source_frames_per_output);
                const auto source_render = voice.renderer.render(source, raw, raw_frames);
                source_active = source_render.active;
                rendered_raw_frames = source_render.source_backed_frames;
            }
            const auto heritage_processed =
                !process_heritage_voice ||
                (draining_heritage_tail
                     ? heritage_.process_voice_tail(voice_index, heritage_plan, voice_scratch)
                     : heritage_.process_voice(voice_index, heritage_plan, voice_scratch,
                                               static_cast<std::size_t>(rendered_raw_frames),
                                               !source_active));
            if (process_heritage_voice && !heritage_processed) {
                reset_voice(voice);
                continue;
            }

            voice.adsr.set_params(params.adsr);
            bool voice_finished = false;
            const auto live_valid_frames =
                process_heritage_voice && heritage_.live_cyclic_active()
                    ? heritage_.last_valid_voice_output_frames(voice_index)
                    : rendered_raw_frames;
            const auto active_frames =
                draining_heritage_tail || source_active
                    ? voice_frames
                    : voice_activity_frames(
                          voice_frames, heritage_.live_cyclic_active() ? voice_frames : raw_frames,
                          live_valid_frames,
                          process_heritage_voice ? heritage_.voice_tail_output_frames() : 0);
            for (std::uint32_t i = 0; i < active_frames; ++i) {
                const float env = voice.adsr.next();
                if (env <= 0.0001f && voice.released) {
                    voice_finished = true;
                    break;
                }

                if (heritage_.bus_processing_required())
                    bus_voice_activity_[start_frame + rendered + i] = 1;

                const float scale = env * voice.velocity * params.gain;
                for (std::uint32_t ch = 0; ch < output_channels; ++ch) {
                    output.channel_ptr(ch)[start_frame + rendered + i] +=
                        voice_scratch_[ch][i] * scale;
                }
            }

            if (voice_finished) {
                reset_voice(voice);
            } else if (draining_heritage_tail) {
                voice.heritage_tail_frames_remaining -= voice_frames;
                if (voice.heritage_tail_frames_remaining == 0)
                    reset_voice(voice);
            } else if (!source_active) {
                if (!process_heritage_voice || heritage_.voice_tail_output_frames() == 0) {
                    reset_voice(voice);
                } else {
                    voice.heritage_source_exhausted = true;
                    const auto consumed = tail_frames_consumed(
                        voice_frames, heritage_.live_cyclic_active() ? voice_frames : raw_frames,
                        live_valid_frames);
                    const auto tail = heritage_.voice_tail_output_frames();
                    voice.heritage_tail_frames_remaining = tail > consumed ? tail - consumed : 0;
                    if (voice.heritage_tail_frames_remaining == 0)
                        reset_voice(voice);
                }
            }
        }
        rendered += chunk;
    }
}

void PulpSamplerProcessor::render_streamed_voice(std::size_t voice_index, SamplerVoice& voice,
                                                 audio::BufferView<float>& output,
                                                 std::uint32_t output_start, std::uint32_t frames,
                                                 std::uint32_t output_channels,
                                                 const RenderParams& params) noexcept {
    const bool process_heritage_voice = heritage_.voice_processing_required();
    const bool draining_heritage_tail = voice.heritage_source_exhausted;
    const auto voice_frames = draining_heritage_tail
                                  ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                        frames, voice.heritage_tail_frames_remaining))
                                  : frames;
    SamplerHeritageRuntime::VoiceProcessPlan heritage_plan;
    std::uint32_t raw_frames = voice_frames;
    audio::BufferView<float> raw(voice_scratch_ptrs_.data(), output_channels, voice_frames);
    if (draining_heritage_tail && process_heritage_voice) {
        if (!heritage_.plan_voice_tail(voice_index, voice_frames,
                                       voice.heritage_pitch_factor,
                                       voice.heritage_clock_multiplier,
                                       heritage_plan)) {
            queue_voice_cancellation(voice_index, voice.requester);
            reset_voice(voice);
            return;
        }
        raw_frames = static_cast<std::uint32_t>(heritage_plan.input_frames);
        raw = heritage_.voice_dry_buffer(voice_index, raw_frames);
        clear_output(raw);
    }
    if (draining_heritage_tail) {
        audio::BufferView<float> processed(voice_scratch_ptrs_.data(), output_channels,
                                           voice_frames);
        if (!heritage_.process_voice_tail(voice_index, heritage_plan, processed)) {
            reset_voice(voice);
            return;
        }
        voice.adsr.set_params(params.adsr);
        bool voice_finished = false;
        for (std::uint32_t frame = 0; frame < voice_frames; ++frame) {
            const float envelope = voice.adsr.next();
            if (envelope <= 0.0001f && voice.released) {
                voice_finished = true;
                break;
            }
            if (heritage_.bus_processing_required())
                bus_voice_activity_[output_start + frame] = 1;
            const float scale = envelope * voice.velocity * params.gain;
            for (std::uint32_t channel = 0; channel < output_channels; ++channel) {
                output.channel_ptr(channel)[output_start + frame] +=
                    voice_scratch_[channel][frame] * scale;
            }
        }
        voice.heritage_tail_frames_remaining -= voice_frames;
        if (voice_finished || voice.heritage_tail_frames_remaining == 0)
            reset_voice(voice);
        return;
    }

    const auto pitch_ratio = key_map_.pitch_ratio_for_note(voice.note, params.pitch_semitones);
    const auto previous_pitch_factor = voice.heritage_pitch_factor;
    const auto previous_clock_multiplier = voice.heritage_clock_multiplier;
    const auto previous_live_consumption_factor = voice.heritage_live_consumption_factor;
    const auto requested_clock_multiplier = heritage_.clock_automation_active()
        ? params.heritage_clock_multiplier
        : 1.0;
    const auto pitch_contract = pitch_rate_contract(
        pitch_ratio, requested_clock_multiplier);
    if (pitch_contract == StreamRateContract::LegacyRejected) {
        streaming_->record_voice_outcome(
            audio::SampleStreamVoiceOutcomeClass::InvalidRenderContract);
        queue_voice_cancellation(voice_index, voice.requester);
        reset_voice(voice);
        return;
    }
    const auto requested_source_frames_per_output =
        pitch_ratio * static_cast<double>(voice.streamed_asset.sample_rate) /
        static_cast<double>(host_sample_rate_);
    const auto unpitched_source_frames_per_output =
        static_cast<double>(voice.streamed_asset.sample_rate) /
        static_cast<double>(host_sample_rate_);
    auto source_frames_per_output = heritage_.pitch_active() ? unpitched_source_frames_per_output
                                                             : requested_source_frames_per_output;
    auto effective_pitch_factor = pitch_ratio;
    auto effective_clock_multiplier = requested_clock_multiplier;
    auto effective_live_consumption_factor = heritage_.active_live_source_consumption_ratio();
    if (!voice.stream_contract_fade_pending &&
        pitch_contract == StreamRateContract::HeritageRejected) {
        heritage_.record_rate_automation_rejection();
        voice.stream_contract_fade_pending = true;
        voice.stream_contract_fade_position = 0;
        source_frames_per_output = voice.stream_playback_rate;
        effective_pitch_factor = voice.heritage_pitch_factor;
        effective_clock_multiplier = voice.heritage_clock_multiplier;
        effective_live_consumption_factor = voice.heritage_live_consumption_factor;
    } else if (!voice.stream_contract_fade_pending) {
        const auto aggregate_contract = stream_rate_contract(
            voice.streamed_asset, requested_source_frames_per_output, params, voice_index);
        if (aggregate_contract != StreamRateContract::Allowed) {
            source_frames_per_output = voice.stream_playback_rate;
            effective_pitch_factor = voice.heritage_pitch_factor;
            effective_clock_multiplier = voice.heritage_clock_multiplier;
            effective_live_consumption_factor = voice.heritage_live_consumption_factor;
            if (aggregate_contract == StreamRateContract::HeritageRejected) {
                heritage_.record_rate_automation_rejection();
            } else {
                streaming_->record_aggregate_rate_automation_rejection();
            }
            voice.stream_contract_fade_pending = true;
            voice.stream_contract_fade_position = 0;
        }
    } else if (voice.stream_contract_fade_pending) {
        source_frames_per_output = voice.stream_playback_rate;
        effective_pitch_factor = voice.heritage_pitch_factor;
        effective_clock_multiplier = voice.heritage_clock_multiplier;
        effective_live_consumption_factor = voice.heritage_live_consumption_factor;
    }
    voice.heritage_pitch_factor = effective_pitch_factor;
    voice.heritage_clock_multiplier = effective_clock_multiplier;
    voice.heritage_live_consumption_factor = effective_live_consumption_factor;

    if (process_heritage_voice) {
        if (!heritage_.plan_voice(voice_index, voice_frames,
                                  effective_pitch_factor,
                                  effective_clock_multiplier,
                                  heritage_plan)) {
            queue_voice_cancellation(voice_index, voice.requester);
            reset_voice(voice);
            return;
        }
        raw_frames = static_cast<std::uint32_t>(heritage_plan.input_frames);
        raw = heritage_.voice_dry_buffer(voice_index, raw_frames);
        clear_output(raw);
    }

    const bool adapt_stream_channels =
        process_heritage_voice && voice.streamed_asset.channels > output_channels;
    std::array<float*, kMaxOutputChannels> source_ptrs{};
    for (std::size_t channel = 0; channel < voice.streamed_asset.channels; ++channel) {
        source_ptrs[channel] = adapt_stream_channels
                                   ? stream_source_scratch_ptrs_[channel]
                                   : (process_heritage_voice ? raw.channel_ptr(channel)
                                                             : voice_scratch_ptrs_[channel]);
    }
    audio::BufferView<float> source_scratch(source_ptrs.data(), voice.streamed_asset.channels,
                                            raw_frames);
    const auto interpolation = prepared_pitch_source_interpolation(
        params.interpolation, source_frames_per_output, requested_source_frames_per_output);
    if (!interpolation.valid()) {
        streaming_->record_voice_outcome(
            audio::SampleStreamVoiceOutcomeClass::InvalidRenderContract);
        queue_voice_cancellation(voice_index, voice.requester);
        reset_voice(voice);
        return;
    }

    const bool rate_changed = voice.stream_playback_rate != source_frames_per_output;
    const bool consumption_changed =
        previous_pitch_factor != voice.heritage_pitch_factor ||
        previous_clock_multiplier != voice.heritage_clock_multiplier ||
        previous_live_consumption_factor != voice.heritage_live_consumption_factor;
    const bool interpolation_changed =
        !audio::same_sample_interpolation(voice.stream_reader.interpolation(), interpolation);
    if (rate_changed || interpolation_changed || consumption_changed) {
        if ((rate_changed && !voice.stream_reader.set_playback_rate(voice.streamed_asset,
                                                                    source_frames_per_output)) ||
            !voice.stream_reader.set_interpolation(voice.streamed_asset, interpolation) ||
            !voice.lookahead_reader.set_interpolation(voice.streamed_asset, interpolation) ||
            !voice.lookahead_reader.synchronize_cursor(voice.streamed_asset,
                                                       voice.stream_reader.cursor())) {
            queue_voice_cancellation(voice_index, voice.requester);
            reset_voice(voice);
            return;
        }
        voice.stream_playback_rate = source_frames_per_output;
        voice.lookahead_lead_source_frames = 0.0;
        voice.pending_lookahead = {};
        voice.pending_demand_index = 0;
        voice.pending_refresh_index = 0;
        voice.pending_lookahead_valid = false;
    }

    enqueue_forward_stream_boundary(voice, source_frames_per_output);

    const bool holding_stream_attack = voice.stream_attack_pending;
    if (holding_stream_attack) {
        if (!prepare_reverse_attack_horizon(voice, source_frames_per_output)) {
            voice.stream_reader.mark_held_starvation(raw_frames);
            streaming_->record_voice_outcome(
                audio::SampleStreamVoiceOutcomeClass::ServiceStarvation, raw_frames);
            return;
        }
#if defined(PULP_SAMPLER_TEST_HOOKS)
        retire_reverse_attack_page_after_horizon_for_test(voice);
#endif
    }

    enqueue_stream_lookahead(voice, raw_frames, source_frames_per_output);
    audio::SampleStreamVoiceBlockResult rendered{.supply = audio::SampleStreamVoiceSupply::Ready,
                                                 .outcome =
                                                     audio::SampleStreamVoiceOutcomeClass::None,
                                                 .ready_output_frames = 0};
    if (raw_frames != 0) {
        const auto plan = voice.stream_reader.plan_block(voice.streamed_asset, raw_frames,
                                                         stream_output_sample_rate_,
                                                         stream_consumption_declaration(voice));
        if (plan.supply == audio::SampleStreamVoiceSupply::InvalidContract ||
            plan.supply == audio::SampleStreamVoiceSupply::StaleGeneration) {
            streaming_->record_voice_outcome(
                plan.supply == audio::SampleStreamVoiceSupply::StaleGeneration
                    ? audio::SampleStreamVoiceOutcomeClass::StaleGeneration
                    : audio::SampleStreamVoiceOutcomeClass::InvalidPreloadContract);
            queue_voice_cancellation(voice_index, voice.requester);
            reset_voice(voice);
            return;
        }
        if (holding_stream_attack && !stream_plan_pages_ready(plan)) {
            voice.stream_reader.mark_held_starvation(raw_frames);
            streaming_->record_voice_outcome(
                audio::SampleStreamVoiceOutcomeClass::ServiceStarvation, raw_frames);
            return;
        }
        if (holding_stream_attack)
            voice.stream_attack_pending = false;
        rendered = voice.stream_reader.render_block(voice.streamed_asset, plan, source_scratch);
        streaming_->record_voice_outcome(rendered.outcome,
                                         rendered.supply == audio::SampleStreamVoiceSupply::Starved
                                             ? raw_frames - rendered.ready_output_frames
                                             : 0);
        if (rendered.supply == audio::SampleStreamVoiceSupply::InvalidContract ||
            rendered.supply == audio::SampleStreamVoiceSupply::StaleGeneration) {
            queue_voice_cancellation(voice_index, voice.requester);
            reset_voice(voice);
            return;
        }
    }
    voice.lookahead_lead_source_frames -=
        static_cast<double>(raw_frames) * source_frames_per_output;
    const bool source_exhausted = rendered.supply == audio::SampleStreamVoiceSupply::EndOfSource ||
                                  !voice.stream_reader.active();
    if (process_heritage_voice) {
        if (adapt_stream_channels) {
            for (std::size_t channel = 0; channel < output_channels; ++channel) {
                std::copy_n(stream_source_scratch_[channel].data(), raw_frames,
                            raw.channel_ptr(channel));
            }
        }
        audio::BufferView<float> processed(voice_scratch_ptrs_.data(), output_channels,
                                           voice_frames);
        if (!heritage_.process_voice(voice_index, heritage_plan, processed,
                                     rendered.ready_output_frames, source_exhausted)) {
            queue_voice_cancellation(voice_index, voice.requester);
            reset_voice(voice);
            return;
        }
    }
    voice.adsr.set_params(params.adsr);
    bool voice_finished = false;
    const auto live_valid_frames = process_heritage_voice && heritage_.live_cyclic_active()
                                       ? heritage_.last_valid_voice_output_frames(voice_index)
                                       : rendered.ready_output_frames;
    const auto active_frames =
        !source_exhausted
            ? voice_frames
            : voice_activity_frames(
                  voice_frames, heritage_.live_cyclic_active() ? voice_frames : raw_frames,
                  live_valid_frames,
                  process_heritage_voice ? heritage_.voice_tail_output_frames() : 0);
    for (std::uint32_t frame = 0; frame < active_frames; ++frame) {
        const float envelope = voice.adsr.next();
        if (envelope <= 0.0001f && voice.released) {
            voice_finished = true;
            break;
        }
        if (heritage_.bus_processing_required())
            bus_voice_activity_[output_start + frame] = 1;
        float contract_gain = 1.0f;
        if (voice.stream_contract_fade_pending) {
            constexpr float kHalfPi = 1.57079632679489661923f;
            constexpr std::uint32_t kContractFadeFrames = 64;
            const auto fade_position =
                std::min(voice.stream_contract_fade_position, kContractFadeFrames - 1);
            contract_gain = std::cos(kHalfPi * static_cast<float>(fade_position) /
                                     static_cast<float>(kContractFadeFrames - 1));
            ++voice.stream_contract_fade_position;
        }
        const float scale = envelope * voice.velocity * params.gain * contract_gain;
        for (std::uint32_t channel = 0; channel < output_channels; ++channel) {
            float sample = 0.0f;
            if (voice.streamed_asset.channels == 1) {
                sample = voice_scratch_[0][frame];
            } else if (channel < voice.streamed_asset.channels) {
                sample = voice_scratch_[channel][frame];
            }
            output.channel_ptr(channel)[output_start + frame] += sample * scale;
        }
    }

    const bool contract_fade_complete =
        voice.stream_contract_fade_pending && voice.stream_contract_fade_position >= 64;
    if (voice_finished || contract_fade_complete) {
        queue_voice_cancellation(voice_index, voice.requester);
        reset_voice(voice);
    } else if (source_exhausted) {
        queue_voice_cancellation(voice_index, voice.requester);
        const auto tail_frames = heritage_.voice_tail_output_frames();
        if (!process_heritage_voice || tail_frames == 0) {
            reset_voice(voice);
        } else {
            voice.heritage_source_exhausted = true;
            const auto consumed = tail_frames_consumed(
                voice_frames, heritage_.live_cyclic_active() ? voice_frames : raw_frames,
                live_valid_frames);
            voice.heritage_tail_frames_remaining =
                tail_frames > consumed ? tail_frames - consumed : 0;
            if (voice.heritage_tail_frames_remaining == 0)
                reset_voice(voice);
        }
    }
}

bool PulpSamplerProcessor::prepare_reverse_attack_horizon(SamplerVoice& voice, double) noexcept {
    const auto& asset = voice.streamed_asset;
    if (asset.fully_resident())
        return true;
    if (!asset.valid() || !asset.has_stream_source || asset.stream_source.window == nullptr ||
        asset.stream_source.page_frames == 0 || asset.preload_frames == 0) {
        return false;
    }

    const auto page_frames = asset.stream_source.page_frames;
    const auto tail_frame = asset.total_frames - 1;
    const auto first_frame =
        asset.total_frames > asset.preload_frames ? asset.total_frames - asset.preload_frames : 0;
    const auto first_page = first_frame / page_frames;
    const auto last_page = tail_frame / page_frames;
    bool ready = true;
    for (auto page = first_page; page <= last_page; ++page) {
        const auto page_start = page * page_frames;
        const auto probe_frame = std::max(first_frame, page_start);
        if (asset.stream_source.window
                ->ready_page_for_frame(asset.source.source_generation, probe_frame)
                .valid) {
            continue;
        }
        ready = false;
        const auto page_end = page == last_page ? asset.total_frames : (page + 1) * page_frames;
        const auto consumption_frames_per_second =
            stream_consumption_declaration(voice).source_frames_per_second;
#if defined(PULP_SAMPLER_TEST_HOOKS)
        last_stream_demand_fps_for_test_ = consumption_frames_per_second;
#endif
        (void)streaming_->command_inbox().demand_page({
            .source = asset.source,
            .requester = voice.requester,
            .page_index = page,
            .resident_source_frames = tail_frame - (page_end - 1),
            .consumption_frames_per_second = consumption_frames_per_second,
            .demand_class = audio::SampleStreamDemandClass::Attack,
        });
    }
    return ready;
}

bool PulpSamplerProcessor::stream_plan_pages_ready(
    const audio::SampleStreamLoopBlockPlan& plan) noexcept {
    if (plan.supply != audio::SampleStreamVoiceSupply::Ready ||
        plan.demand_count > plan.ready_pages.size()) {
        return false;
    }
    for (std::uint32_t index = 0; index < plan.demand_count; ++index) {
        if (!plan.ready_pages[index].valid)
            return false;
    }
    return true;
}

#if defined(PULP_SAMPLER_TEST_HOOKS)

void PulpSamplerProcessor::retire_reverse_attack_page_after_horizon_for_test(
    const SamplerVoice& voice) noexcept {
    if (!retire_reverse_attack_after_horizon_for_test_)
        return;
    retire_reverse_attack_after_horizon_for_test_ = false;
    const auto& asset = voice.streamed_asset;
    if (!asset.valid() || asset.stream_source.window == nullptr || asset.total_frames == 0) {
        return;
    }
    const auto tail = asset.stream_source.window->ready_page_for_frame(
        asset.source.source_generation, asset.total_frames - 1);
    if (tail.valid) {
        (void)asset.stream_source.window->retire_page(tail.page_index, 1);
    }
}
#endif

void PulpSamplerProcessor::enqueue_stream_lookahead(SamplerVoice& voice, std::uint32_t frames,
                                                    double source_frames_per_output) noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
    lookahead_plans_last_callback_for_test_ = 0;
#endif
    if (voice.pending_lookahead_valid) {
        const auto lead = stream_lead_source_frames(voice.lookahead_lead_source_frames);
        const auto refreshed = voice.lookahead_reader.enqueue_demands(
            voice.pending_lookahead, streaming_->command_inbox(), voice.pending_refresh_index, lead,
            voice.pending_demand_index);
        voice.pending_refresh_index = refreshed.next_demand_index;
        if (!refreshed.complete)
            return;
        const auto retry = voice.lookahead_reader.enqueue_demands(
            voice.pending_lookahead, streaming_->command_inbox(), voice.pending_demand_index, lead);
        voice.pending_demand_index = retry.next_demand_index;
        if (!retry.complete) {
            voice.pending_refresh_index = 0;
            return;
        }
        if (!voice.lookahead_reader.commit_planned_timeline(voice.streamed_asset,
                                                            voice.pending_lookahead)) {
            voice.pending_lookahead_valid = false;
            voice.pending_demand_index = 0;
            return;
        }
        voice.lookahead_lead_source_frames +=
            static_cast<double>(voice.pending_lookahead.output_frames) * source_frames_per_output;
        voice.pending_lookahead_valid = false;
        voice.pending_demand_index = 0;
        voice.pending_refresh_index = 0;
    }

    if (voice.streamed_asset.fully_resident() || !voice.streamed_asset.has_preload_contract) {
        return;
    }
    const auto planning_frames = std::max(frames, maximum_stream_block_frames_);
    const auto& contract = voice.streamed_asset.preload_contract;
    const auto latency_seconds = contract.certified_io_latency_seconds +
                                 contract.scheduler_margin_seconds +
                                 contract.decoder_latency_seconds;
    const auto consumption = stream_consumption_declaration(voice);
    const auto plan_advance = static_cast<double>(planning_frames) * source_frames_per_output;
    const auto contract_lead = latency_seconds * consumption.source_frames_per_second +
                               plan_advance + source_frames_per_output;
    const auto resident_horizon = voice.streamed_asset.preload_frames > 0
                                      ? static_cast<double>(voice.streamed_asset.preload_frames - 1)
                                      : 0.0;
    const auto bounded_preload_lead =
        std::min(resident_horizon, resident_horizon * source_frames_per_output) + plan_advance;
    const auto target_lead = std::max(contract_lead, bounded_preload_lead);
    if (!(plan_advance > 0.0) || !std::isfinite(plan_advance) || !std::isfinite(target_lead)) {
        return;
    }
    if (voice.lookahead_lead_source_frames >= target_lead)
        return;
    const auto required_plans_value =
        std::ceil((target_lead - voice.lookahead_lead_source_frames) / plan_advance);
    if (!(required_plans_value > 0.0) || !std::isfinite(required_plans_value) ||
        required_plans_value > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return;
    }
    const auto required_plans = static_cast<std::uint64_t>(required_plans_value);
    constexpr std::uint64_t kPlanBudgetPerCallback = 8;
    const auto plans_this_callback = std::min(required_plans, kPlanBudgetPerCallback);
    for (std::uint64_t iteration = 0; iteration < plans_this_callback; ++iteration) {
#if defined(PULP_SAMPLER_TEST_HOOKS)
        ++lookahead_plans_last_callback_for_test_;
#endif
        auto plan = voice.lookahead_reader.plan_block(voice.streamed_asset, planning_frames,
                                                      stream_output_sample_rate_, consumption,
                                                      audio::SampleStreamDemandClass::Sustain);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (plan.demand_count != 0) {
            last_lookahead_demand_fps_for_test_ = plan.demands[0].consumption_frames_per_second;
        }
#endif
        if (plan.supply != audio::SampleStreamVoiceSupply::Ready)
            return;
        const auto queued = voice.lookahead_reader.enqueue_demands(
            plan, streaming_->command_inbox(), 0,
            stream_lead_source_frames(voice.lookahead_lead_source_frames));
        if (!queued.complete) {
            voice.pending_lookahead = plan;
            voice.pending_demand_index = queued.next_demand_index;
            voice.pending_refresh_index = 0;
            voice.pending_lookahead_valid = true;
            return;
        }
        if (!voice.lookahead_reader.commit_planned_timeline(voice.streamed_asset, plan))
            return;
        voice.lookahead_lead_source_frames += plan_advance;
    }
}

std::uint64_t PulpSamplerProcessor::stream_lead_source_frames(double lead) noexcept {
    if (!(lead > 0.0))
        return 0;
    if (!std::isfinite(lead) ||
        lead >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::floor(lead));
}

void PulpSamplerProcessor::enqueue_forward_stream_boundary(SamplerVoice& voice, double) noexcept {
    if (!voice.stream_boundary_pending)
        return;
    const auto& asset = voice.streamed_asset;
    if (!asset.valid() || !asset.has_stream_source || asset.stream_source.page_frames == 0 ||
        asset.preload_frames >= asset.total_frames) {
        voice.stream_boundary_pending = false;
        voice.stream_boundary_demand_index = 0;
        return;
    }

    const auto page_frames = asset.stream_source.page_frames;
    const auto first_page = asset.preload_frames / page_frames;
    const auto guarded_frame =
        std::min(asset.total_frames - 1,
                 asset.preload_frames + asset.preload_contract.interpolation_guard_frames);
    const auto last_page = guarded_frame / page_frames;
    const auto demand_count = static_cast<std::uint32_t>(last_page - first_page + 1);
    while (voice.stream_boundary_demand_index < demand_count) {
        const auto page = first_page + voice.stream_boundary_demand_index;
        const auto page_start = page * page_frames;
        const auto first_use = std::max(asset.preload_frames, page_start);
        const auto distance =
            std::max(0.0, static_cast<double>(first_use) - voice.stream_reader.cursor().position());
        const auto consumption_frames_per_second =
            stream_consumption_declaration(voice).source_frames_per_second;
#if defined(PULP_SAMPLER_TEST_HOOKS)
        last_stream_demand_fps_for_test_ = consumption_frames_per_second;
#endif
        if (streaming_->command_inbox().demand_page({
                .source = asset.source,
                .requester = voice.requester,
                .page_index = page,
                .resident_source_frames = stream_lead_source_frames(distance),
                .consumption_frames_per_second = consumption_frames_per_second,
                .demand_class = audio::SampleStreamDemandClass::Attack,
            }) != audio::SampleStreamCommandPushStatus::Enqueued) {
            return;
        }
        ++voice.stream_boundary_demand_index;
    }
    voice.stream_boundary_pending = false;
    voice.stream_boundary_demand_index = 0;
}

void PulpSamplerProcessor::trigger_note(int note, float velocity,
                                        const SamplerPublishedSource& source,
                                        const RenderParams& params) {
    SamplerVoice* target = nullptr;
    std::size_t target_index = 0;
    for (std::size_t index = 0; index < kMaxVoices; ++index) {
        if (!voices_[index].active && !pending_cancellations_[index].valid) {
            target = &voices_[index];
            target_index = index;
            break;
        }
    }
    if (target == nullptr) {
        for (std::size_t index = 0; index < kMaxVoices; ++index) {
            if (pending_cancellations_[index].valid)
                continue;
            target = &voices_[index];
            target_index = index;
            break;
        }
    }
    if (target == nullptr)
        return;

    if (source.kind == SamplerPublishedSourceKind::Streamed) {
        const auto pitch_ratio = key_map_.pitch_ratio_for_note(note, params.pitch_semitones);
        const auto pitch_contract = pitch_rate_contract(
            pitch_ratio, heritage_.clock_automation_active()
                ? params.heritage_clock_multiplier
                : 1.0);
        if (pitch_contract != StreamRateContract::Allowed) {
            if (pitch_contract == StreamRateContract::HeritageRejected)
                heritage_.record_rate_admission_rejection();
            return;
        }
        auto& requester_generation = requester_generations_[target_index];
        if (++requester_generation == 0)
            ++requester_generation;
        const auto base_source_frames_per_output =
            pitch_ratio * static_cast<double>(source.streamed.sample_rate) /
            static_cast<double>(host_sample_rate_);
        const auto* mip =
            select_streamed_mip(source, params.interpolation, base_source_frames_per_output,
                                params.loop, params.reverse);
        const auto& selected = mip == nullptr ? source.streamed : mip->asset;
        const auto requested_source_frames_per_output =
            pitch_ratio * selected.sample_rate / static_cast<double>(host_sample_rate_);
        const auto source_frames_per_output =
            heritage_.pitch_active() ? selected.sample_rate / static_cast<double>(host_sample_rate_)
                                     : requested_source_frames_per_output;
        const auto aggregate_contract = stream_rate_contract(
            selected, requested_source_frames_per_output, params, target_index);
        if (aggregate_contract != StreamRateContract::Allowed) {
            if (aggregate_contract == StreamRateContract::HeritageRejected) {
                heritage_.record_rate_admission_rejection();
            } else {
                streaming_->record_aggregate_rate_admission_rejection();
            }
            return;
        }
        if (target->streamed) {
            queue_voice_cancellation(target_index, target->requester);
            if (pending_cancellations_[target_index].valid)
                return;
        }
        const auto region =
            make_region(selected.total_frames, selected.sample_rate, params.loop, params.reverse);
        reset_voice(*target);
        if (!target->start_streamed(
                note, velocity, host_sample_rate_, selected, region, source_frames_per_output,
                prepared_pitch_source_interpolation(params.interpolation, source_frames_per_output,
                                                    requested_source_frames_per_output),
                {target_index + 1, requester_generation}, source.selection_generation,
                mip == nullptr ? 0 : mip->octave)) {
            reset_voice(*target);
        } else {
            target->heritage_pitch_factor = pitch_ratio;
            target->heritage_clock_multiplier =
                heritage_.clock_automation_active()
                    ? params.heritage_clock_multiplier
                    : 1.0;
            target->heritage_live_consumption_factor =
                heritage_.active_live_source_consumption_ratio();
        }
        return;
    }

    const auto& sample = source.resident;
    if (target->streamed) {
        queue_voice_cancellation(target_index, target->requester);
        if (pending_cancellations_[target_index].valid)
            return;
    }
    const auto base_speed = playback_speed(note, sample, params);
    if (!(base_speed > 0.0))
        return;
    const auto mip =
        select_resident_mip(source, params.interpolation, base_speed, params.loop, params.reverse);
    const auto selected_frames = mip.valid() ? mip.frames : sample.num_frames;
    const auto selected_rate = mip.valid() ? mip.sample_rate : sample.sample_rate;
    const auto region = make_region(selected_frames, selected_rate, params.loop, params.reverse);
    const auto pitch_ratio = key_map_.pitch_ratio_for_note(note, params.pitch_semitones);
    const auto clock_multiplier = heritage_.clock_automation_active()
        ? params.heritage_clock_multiplier
        : 1.0;
    if (!heritage_.runtime_rate_factors_supported(
            pitch_ratio, clock_multiplier)) {
        heritage_.record_rate_admission_rejection();
        return;
    }
    const auto speed = heritage_.pitch_active()
                           ? selected_rate / static_cast<double>(host_sample_rate_)
                           : playback_speed(note, selected_rate, params);
    const auto interpolation =
        prepared_pitch_source_interpolation(params.interpolation, speed, base_speed);
    if (!interpolation.valid())
        return;
    reset_voice(*target);
    if (!target->start(note, velocity, speed, host_sample_rate_, sample, mip, region,
                       selected_frames, interpolation)) {
        reset_voice(*target);
        return;
    }
    target->heritage_pitch_factor = pitch_ratio;
    target->heritage_clock_multiplier = clock_multiplier;
    target->heritage_live_consumption_factor = heritage_.active_live_source_consumption_ratio();
    target->selection_generation = source.selection_generation;
}

void PulpSamplerProcessor::queue_voice_cancellation(
    std::size_t voice_index, audio::SampleStreamRequesterToken requester) noexcept {
    if (requester.requester_id == 0 || requester.requester_generation == 0)
        return;
    if (streaming_->command_inbox().cancel_requester(requester) ==
        audio::SampleStreamCommandPushStatus::Enqueued) {
        return;
    }
    auto& pending = pending_cancellations_[voice_index];
    if (!pending.valid)
        pending = {requester, true};
}

void PulpSamplerProcessor::flush_pending_cancellations() noexcept {
    for (auto& pending : pending_cancellations_) {
        if (!pending.valid)
            continue;
        if (streaming_->command_inbox().cancel_requester(pending.requester) !=
            audio::SampleStreamCommandPushStatus::Enqueued) {
            return;
        }
        pending = {};
    }
}

void PulpSamplerProcessor::release_note(int note) {
    for (auto& voice : voices_) {
        if (voice.active && voice.note == note && !voice.released) {
            voice.release();
        }
    }
}

void PulpSamplerProcessor::reset_all_voices() noexcept {
    for (std::size_t index = 0; index < kMaxVoices; ++index) {
        auto& voice = voices_[index];
        if (voice.active && voice.streamed)
            queue_voice_cancellation(index, voice.requester);
        reset_voice(voice);
    }
    flush_pending_cancellations();
}

} // namespace pulp::examples
