// visualization_bridge.cpp — out-of-line VisualizationBridge::process().
//
// The public header (pulp/view/visualization_bridge.hpp) forward-declares
// pulp::render::bench::PerfCounters so view consumers don't pull the
// render include tree when PULP_BENCHMARK is on (Codex P1 on PR #526 /
// #500). The PerfCounters FIELDS (atomic<double>::fetch_add) still need
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
                                  int num_samples) {
    // Multi-channel metering
    meter_.process(channels, num_channels, num_samples);
#ifdef PULP_BENCHMARK
    {
        const double t0 = render::bench::now_us();
        meter_buf_.write(meter_.snapshot());
        if (bench_counters_) {
            bench_counters_->triplebuffer_publish_total_us.fetch_add(
                render::bench::now_us() - t0, std::memory_order_relaxed);
        }
    }
#else
    meter_buf_.write(meter_.snapshot());
#endif

    // STFT on first channel (or mono mix for multi-channel)
    if (num_channels > 0 && num_samples > 0) {
        bool new_frame = stft_.push_samples(channels[0], num_samples);

        if (new_frame) {
            // Publish spectrum
            SpectrumData spec;
            auto db = stft_.latest_magnitude_db(-120.0f);
            spec.num_bins = std::min(static_cast<int>(db.size()),
                                     static_cast<int>(SpectrumData::kMaxBins));
#ifdef PULP_BENCHMARK
            {
                const double t_copy = render::bench::now_us();
                std::copy_n(db.data(), spec.num_bins, spec.magnitude_db.data());
                if (bench_counters_) {
                    bench_counters_->audio_copy_total_us.fetch_add(
                        render::bench::now_us() - t_copy,
                        std::memory_order_relaxed);
                }
            }
            {
                const double t_pub = render::bench::now_us();
                spectrum_buf_.write(spec);
                if (bench_counters_) {
                    bench_counters_->triplebuffer_publish_total_us.fetch_add(
                        render::bench::now_us() - t_pub,
                        std::memory_order_relaxed);
                }
            }
#else
            std::copy_n(db.data(), spec.num_bins, spec.magnitude_db.data());
            spectrum_buf_.write(spec);
#endif
        }
    }

    // Waveform capture (ring buffer of latest samples from channel 0)
    if (config_.capture_waveform && num_channels > 0) {
        for (int i = 0; i < num_samples; ++i) {
            waveform_ring_[waveform_pos_] = channels[0][i];
            waveform_pos_ = (waveform_pos_ + 1) % waveform_length_;
        }

        WaveformData wd;
        wd.num_samples = waveform_length_;
        wd.num_channels = num_channels;
#ifdef PULP_BENCHMARK
        {
            const double t_copy = render::bench::now_us();
            // Copy in order from oldest to newest
            for (int i = 0; i < waveform_length_; ++i) {
                wd.samples[i] = waveform_ring_[(waveform_pos_ + i) % waveform_length_];
            }
            if (bench_counters_) {
                bench_counters_->audio_copy_total_us.fetch_add(
                    render::bench::now_us() - t_copy,
                    std::memory_order_relaxed);
            }
        }
        {
            const double t_pub = render::bench::now_us();
            waveform_buf_.write(wd);
            if (bench_counters_) {
                bench_counters_->triplebuffer_publish_total_us.fetch_add(
                    render::bench::now_us() - t_pub,
                    std::memory_order_relaxed);
            }
        }
#else
        // Copy in order from oldest to newest
        for (int i = 0; i < waveform_length_; ++i) {
            wd.samples[i] = waveform_ring_[(waveform_pos_ + i) % waveform_length_];
        }
        waveform_buf_.write(wd);
#endif
    }
}

} // namespace pulp::view
