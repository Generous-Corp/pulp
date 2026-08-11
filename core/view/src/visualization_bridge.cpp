// visualization_bridge.cpp — out-of-line VisualizationBridge::process().
//
// The public header (pulp/view/visualization_bridge.hpp) forward-declares
// pulp::render::bench::PerfCounters so view consumers don't pull the
// render include tree when PULP_BENCHMARK is on. The PerfCounters fields
// (atomic<double>::fetch_add) still need
// the full type, so the process() body lives here and only pulp-view's
// translation unit sees the render header — consumers of
// pulp::view::VisualizationBridge still don't need to link pulp::render.

#include <pulp/view/visualization_bridge.hpp>

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

#include <algorithm>
#include <cmath>

namespace pulp::view {

void VisualizationBridge::process(const float* const* channels,
                                  int num_channels,
                                  int num_samples) noexcept {
    auto publish_meter = [this](const signal::MultiChannelMeterData& data) {
#ifdef PULP_BENCHMARK
        const double t0 = render::bench::now_us();
        meter_buf_.write(data);
        if (bench_counters_) {
            bench_counters_->triplebuffer_publish_total_us.fetch_add(
                render::bench::now_us() - t0, std::memory_order_relaxed);
        }
#else
        meter_buf_.write(data);
#endif
    };

    if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
        publish_meter(signal::MultiChannelMeterData{});
        return;
    }

    // Multi-channel metering
    meter_.process(channels, num_channels, num_samples);
    publish_meter(meter_.snapshot());

    if (!capture_ready_) return;

    // Analyzer topology is invariant between quiescent configure() calls.
    // Meter mismatched host blocks above, but never splice them into one STFT
    // history and then label the result as a different channel layout. Capture
    // stays suspended until poll() acknowledges the discontinuity, preventing
    // later valid audio from being joined to pre-mismatch history.
    const int captured_channels = static_cast<int>(channel_stfts_.size());
    if (captured_channels <= 0) return;
    if (num_channels != captured_channels) {
        discontinuity_generation_.fetch_add(1, std::memory_order_release);
        return;
    }
    for (int ch = 0; ch < num_channels; ++ch) {
        if (channels[ch] == nullptr) {
            discontinuity_generation_.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    if (discontinuity_generation_.load(std::memory_order_acquire)
        != acknowledged_discontinuity_.load(std::memory_order_acquire)) return;

    const audio::BufferView<const float> block(
        channels, static_cast<std::size_t>(captured_channels),
        static_cast<std::size_t>(num_samples));
#ifdef PULP_BENCHMARK
    const double copy_start = render::bench::now_us();
#endif
    (void)capture_.write(block, static_cast<std::uint64_t>(num_samples));
#ifdef PULP_BENCHMARK
    if (bench_counters_) {
        bench_counters_->audio_copy_total_us.fetch_add(
            render::bench::now_us() - copy_start, std::memory_order_relaxed);
    }
#endif
}

void VisualizationBridge::reset_analysis_continuity() {
    for (auto& stft : channel_stfts_) stft.reset();
    waveform_pos_ = 0;
    std::fill(waveform_ring_.begin(), waveform_ring_.end(), 0.0f);
    if (++epoch_ == 0) ++epoch_;
}

bool VisualizationBridge::poll() {
    if (!capture_ready_ || consumer_chunk_frames_ <= 0 || channel_stfts_.empty())
        return false;

    // Rejected/missing-layout blocks are analysis discontinuities. The audio
    // producer suspends capture after signaling one, so this entry-bounded
    // flush cannot discard valid post-gap audio. Acknowledge only the observed
    // generation; a concurrent later discontinuity remains pending.
    const auto discontinuity =
        discontinuity_generation_.load(std::memory_order_acquire);
    if (discontinuity != observed_discontinuity_) {
        const auto available_at_entry = capture_.available_frames();
        (void)capture_.drain(available_at_entry);
        observed_discontinuity_ = discontinuity;
        acknowledged_discontinuity_.store(discontinuity,
                                           std::memory_order_release);
        reset_analysis_continuity();
        return false;
    }

    // A capture overrun is an analysis discontinuity. Discard only the frames
    // visible at entry (bounded even under continuous production), reset the
    // STFT/waveform histories, and leave frames accepted during this flush for
    // the next poll's fresh continuity epoch.
    const auto dropped = capture_.stats().dropped_write_frames;
    const auto available_at_entry = capture_.available_frames();
    if (dropped != observed_dropped_frames_) {
        (void)capture_.drain(available_at_entry);
        observed_dropped_frames_ = dropped;
        reset_analysis_continuity();
        return false;
    }

    const auto entry_frames = config_.max_frames_per_poll > 0
        ? std::min<std::uint64_t>(
              available_at_entry,
              static_cast<std::uint64_t>(config_.max_frames_per_poll))
        : available_at_entry;

    bool spectrum_changed = false;
    bool waveform_changed = false;
    std::uint64_t remaining_frames = entry_frames;
    while (remaining_frames > 0) {
        const auto chunk = static_cast<int>(std::min<std::uint64_t>(
            remaining_frames,
            static_cast<std::uint64_t>(consumer_chunk_frames_)));
        audio::BufferView<float> destination(
            consumer_channels_.data(), consumer_channels_.size(),
            static_cast<std::size_t>(chunk));
        (void)capture_.read(destination, static_cast<std::uint64_t>(chunk));
        remaining_frames -= static_cast<std::uint64_t>(chunk);

        for (std::size_t ch = 0; ch < channel_stfts_.size(); ++ch) {
            float* samples = consumer_channels_[ch];
            for (int i = 0; i < chunk; ++i) {
                if (!std::isfinite(samples[i])) samples[i] = 0.0f;
                samples[i] = std::clamp(samples[i], -1.0e6f, 1.0e6f);
            }
            spectrum_changed = channel_stfts_[ch].push_samples(samples, chunk)
                || spectrum_changed;
        }

        if (config_.capture_waveform && waveform_length_ > 0) {
            const float* first_channel = consumer_channels_.front();
            for (int i = 0; i < chunk; ++i) {
                waveform_ring_[waveform_pos_] = first_channel[i];
                waveform_pos_ = (waveform_pos_ + 1) % waveform_length_;
            }
            waveform_changed = true;
        }
    }

    const int source_channels = config_.num_channels;

    if (spectrum_changed) {
        const int analysis_channels = std::clamp(
            source_channels, 1, static_cast<int>(channel_stfts_.size()));
        SpectrumData spec;
        spec.num_bins = std::min(num_bins(), SpectrumData::kMaxBins);
        spec.epoch = epoch_;
        spec.sequence_number = ++spectrum_sequence_;
        spec.dropped_frames = dropped;
        spec.source_channels = source_channels;
        spec.fft_size = config_.fft_size;
        spec.sample_rate = std::isfinite(config_.sample_rate)
            ? config_.sample_rate : 0.0f;
        spec.floor_db = config_.spectrum_floor_db;

        const double floor_power = std::pow(
            10.0, static_cast<double>(config_.spectrum_floor_db) / 10.0);
        const double finite_floor_power = std::isfinite(floor_power)
            && floor_power > 0.0 ? floor_power : 1.0e-12;
        for (int bin = 0; bin < spec.num_bins; ++bin) {
            double power = 0.0;
            for (int ch = 0; ch < analysis_channels; ++ch) {
                const auto& stft = channel_stfts_[static_cast<std::size_t>(ch)];
                const float magnitude = stft.latest_frame().magnitude[bin];
                const double finite_magnitude = std::isfinite(magnitude)
                    ? static_cast<double>(magnitude) : 0.0;
                power += finite_magnitude * finite_magnitude;
            }
            power /= static_cast<double>(analysis_channels);
            const double db = 10.0 * std::log10(std::max(power, finite_floor_power));
            spec.magnitude_db[bin] = std::isfinite(db)
                ? static_cast<float>(db) : config_.spectrum_floor_db;
        }
#ifdef PULP_BENCHMARK
        const double publish_start = render::bench::now_us();
#endif
        spectrum_buf_.write(spec);
#ifdef PULP_BENCHMARK
        if (bench_counters_) {
            bench_counters_->triplebuffer_publish_total_us.fetch_add(
                render::bench::now_us() - publish_start, std::memory_order_relaxed);
        }
#endif
    }

    if (waveform_changed) {
        WaveformData wd;
        wd.num_samples = waveform_length_;
        wd.num_channels = source_channels;
        wd.sequence_number = ++waveform_sequence_;
        for (int i = 0; i < waveform_length_; ++i) {
            wd.samples[i] = waveform_ring_[(waveform_pos_ + i) % waveform_length_];
        }
#ifdef PULP_BENCHMARK
        const double publish_start = render::bench::now_us();
#endif
        waveform_buf_.write(wd);
#ifdef PULP_BENCHMARK
        if (bench_counters_) {
            bench_counters_->triplebuffer_publish_total_us.fetch_add(
                render::bench::now_us() - publish_start, std::memory_order_relaxed);
        }
#endif
    }

    return spectrum_changed || waveform_changed;
}

} // namespace pulp::view
