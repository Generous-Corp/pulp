#include <pulp/view/audio_inspector_panel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace pulp::view {

namespace {

/// Linear amplitude → dBFS, floored so -inf prints as a readable sentinel.
std::string format_dbfs(float linear) {
    if (linear <= 1.0e-6f) return "-inf";
    float db = 20.0f * std::log10(linear);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", db);
    return buf;
}

const char* stage_name(audio::AudioProbeStage stage) {
    switch (stage) {
        case audio::AudioProbeStage::kProcessorOutput:          return "Processor output";
        case audio::AudioProbeStage::kStandaloneOutputBoundary: return "Standalone boundary";
        case audio::AudioProbeStage::kMeterBridge:              return "Meter bridge";
        case audio::AudioProbeStage::kDeviceCallback:           return "Device callback";
        case audio::AudioProbeStage::kGraphNode:                return "Graph node";
        case audio::AudioProbeStage::kUnknown:                  break;
    }
    return "Unknown stage";
}

}  // namespace

float dbfs_meter_fill(float linear) {
    if (linear <= 1.0e-6f) return 0.0f;
    const float db = 20.0f * std::log10(linear);
    return std::clamp((db - kInspectorMeterFloorDb) / -kInspectorMeterFloorDb,
                      0.0f, 1.0f);
}

// ── AudioWaveformView ───────────────────────────────────────────────────────

AudioWaveformView::AudioWaveformView() {
    set_hit_testable(false);
    set_id("audio-inspector-waveform");
}

void AudioWaveformView::set_samples(const float* samples, int count) {
    count_ = std::clamp(count, 0, kCapacity);
    for (int i = 0; i < count_; ++i) data_[i] = samples[i];
    live_ = true;
}

void AudioWaveformView::clear_samples() {
    count_ = 0;
    live_ = false;
}

void AudioWaveformView::paint(canvas::Canvas& canvas) {
    const Rect b = local_bounds();
    if (b.width <= 0 || b.height <= 0) return;

    // Housing.
    canvas.set_fill_color(canvas::Color{18, 18, 22, 255});
    canvas.fill_rect(b.x, b.y, b.width, b.height);

    const float mid = b.y + b.height * 0.5f;

    // Zero-line: bright when live, dim when stale / no probe.
    canvas.set_stroke_color(live_ ? canvas::Color{70, 80, 95, 255}
                                  : canvas::Color{45, 45, 52, 255});
    canvas.set_line_width(1.0f);
    canvas.stroke_line(b.x, mid, b.x + b.width, mid);

    if (!live_ || count_ < 2) return;  // honest: no trace when not live.

    // Map each sample to a polyline point. Amplitude is clamped to the
    // housing; the trace is a single anti-aliased polyline.
    std::array<canvas::Canvas::Point2D, kCapacity> pts{};
    const float half = b.height * 0.5f;
    const float dx = (count_ > 1) ? b.width / static_cast<float>(count_ - 1) : 0.0f;
    for (int i = 0; i < count_; ++i) {
        float s = std::clamp(data_[i], -1.0f, 1.0f);
        pts[static_cast<std::size_t>(i)] = {b.x + dx * static_cast<float>(i),
                                            mid - s * half};
    }

    canvas.set_stroke_color(canvas::Color{90, 200, 140, 255});
    canvas.set_line_width(1.25f);
    canvas.stroke_path(pts.data(), static_cast<std::size_t>(count_));
}

// ── AudioInspectorPanel ─────────────────────────────────────────────────────

AudioInspectorPanel::AudioInspectorPanel() {
    set_id("audio-inspector-panel");
    build_ui();
    refresh_labels();
}

void AudioInspectorPanel::build_ui() {
    flex().direction = FlexDirection::column;
    flex().padding = 10;
    set_background_color(canvas::Color{26, 26, 32, 255});

    auto make_label = [](const std::string& text, float size, int weight,
                         canvas::Color color) {
        auto l = std::make_unique<Label>(text);
        l->set_font_size(size);
        l->set_font_weight(weight);
        l->set_text_color(color);
        return l;
    };

    const canvas::Color heading{210, 215, 225, 255};
    const canvas::Color body{170, 176, 188, 255};
    const canvas::Color muted{120, 126, 138, 255};

    // Status / availability line.
    {
        auto l = make_label("Audio Inspector", 14.0f, 700, heading);
        l->flex().preferred_height = 20;
        status_label_ = l.get();
        add_child(std::move(l));
    }
    // Observed probe stage.
    {
        auto l = make_label("", 12.0f, 500, body);
        l->flex().preferred_height = 18;
        stage_label_ = l.get();
        add_child(std::move(l));
    }

    // Meter row: peak + RMS bars side by side.
    {
        auto row = std::make_unique<View>();
        row->flex().direction = FlexDirection::row;
        row->flex().preferred_height = 90;
        row->flex().gap = 8;

        auto peak = std::make_unique<Meter>();
        peak->set_orientation(Meter::Orientation::vertical);
        peak->flex().preferred_width = 18;
        peak_meter_ = peak.get();
        row->add_child(std::move(peak));

        auto rms = std::make_unique<Meter>();
        rms->set_orientation(Meter::Orientation::vertical);
        rms->flex().preferred_width = 18;
        rms_meter_ = rms.get();
        row->add_child(std::move(rms));

        auto lvl = make_label("", 11.0f, 500, body);
        lvl->set_multi_line(true);
        lvl->flex().flex_grow = 1;
        level_label_ = lvl.get();
        row->add_child(std::move(lvl));

        add_child(std::move(row));
    }

    // Copied waveform.
    {
        auto wf = std::make_unique<AudioWaveformView>();
        wf->flex().preferred_height = 70;
        waveform_view_ = wf.get();
        add_child(std::move(wf));
    }

    // Content facts (clip / NaN-Inf / silence-run).
    {
        auto l = make_label("", 11.0f, 500, body);
        l->set_multi_line(true);
        l->flex().preferred_height = 34;
        content_label_ = l.get();
        add_child(std::move(l));
    }
    // Phase (L/R level-match + balance).
    {
        auto l = make_label("", 11.0f, 500, body);
        l->flex().preferred_height = 18;
        phase_label_ = l.get();
        add_child(std::move(l));
    }
    // Device / runtime summary.
    {
        auto l = make_label("", 11.0f, 500, muted);
        l->set_multi_line(true);
        l->flex().preferred_height = 34;
        device_label_ = l.get();
        add_child(std::move(l));
    }
}

void AudioInspectorPanel::update(Status status,
                                 const audio::AudioProbeSnapshot& snap,
                                 const audio::AudioStats& stats,
                                 const float* waveform,
                                 int waveform_count) {
    status_ = status;
    stats_ = stats;

    if (status == Status::kNoProbe) {
        // Drop any stale numbers — show an empty, honest state.
        snapshot_ = audio::AudioProbeSnapshot{};
        lr_match_ = 0.0f;
        balance_ = 0.0f;
        if (waveform_view_) waveform_view_->clear_samples();
        refresh_labels();
        return;
    }

    snapshot_ = snap;

    // Stereo channel balance + an L/R level-match ratio, both derived from the
    // snapshot's per-channel RMS. The RT probe summary does NOT retain raw
    // inter-sample L*R products, so this is deliberately NOT a phase/Pearson
    // correlation (which would distinguish a mono signal from an anti-phase
    // L=-R one — both read the same here). A true correlation needs the sample
    // stream and is deferred until the snapshot carries an L*R term (a later
    // probe slice can accumulate it RT-safe, the way peak/RMS already are). For
    // this slice we report what the snapshot honestly supports: balance from
    // the L/R RMS split, and the level-match ratio (equal energy → 1, one side
    // silent → 0). 0 when fewer than two active channels or both sides silent.
    lr_match_ = 0.0f;
    balance_ = 0.0f;
    if (snap.channel_count >= 2) {
        const float l = snap.rms[0];
        const float r = snap.rms[1];
        const float sum = l + r;
        if (sum > 1.0e-9f) {
            balance_ = (r - l) / sum;            // -1 = all left … +1 = all right
            const float lo = std::min(l, r);
            const float hi = std::max(l, r);
            lr_match_ = (hi > 1.0e-9f) ? (lo / hi) : 0.0f;
        }
    }

    if (waveform_view_) {
        if (status == Status::kLive && waveform && waveform_count > 0) {
            waveform_view_->set_samples(waveform, waveform_count);
        } else {
            // Stale or no capture: keep the copied trace honest — no live data.
            waveform_view_->clear_samples();
        }
    }

    // Drive the visual meters with the snapshot levels (instantaneous, no
    // ballistics decay needed for a live readout). Map amplitude through the
    // same dBFS transform the labels use so the bar height matches the dBFS text.
    if (peak_meter_)
        peak_meter_->set_level(dbfs_meter_fill(snapshot_.rms_max),
                               dbfs_meter_fill(snapshot_.peak_max));
    if (rms_meter_)
        rms_meter_->set_level(dbfs_meter_fill(snapshot_.rms_max),
                              dbfs_meter_fill(snapshot_.rms_max));

    refresh_labels();
}

void AudioInspectorPanel::refresh_labels() {
    if (status_label_) {
        switch (status_) {
            case Status::kNoProbe:
                status_label_->set_text("Audio Inspector — no probe");
                status_label_->set_text_color(canvas::Color{200, 150, 90, 255});
                break;
            case Status::kStale:
                status_label_->set_text("Audio Inspector — stale (probe idle)");
                status_label_->set_text_color(canvas::Color{200, 150, 90, 255});
                break;
            case Status::kLive:
                status_label_->set_text("Audio Inspector — live");
                status_label_->set_text_color(canvas::Color{120, 200, 150, 255});
                break;
        }
    }

    if (stage_label_) {
        if (status_ == Status::kNoProbe) {
            stage_label_->set_text("Stage: —");
        } else {
            stage_label_->set_text(std::string("Stage: ") +
                                   stage_name(snapshot_.stage_id));
        }
    }

    if (level_label_) {
        if (status_ == Status::kNoProbe) {
            level_label_->set_text("Peak: —\nRMS: —");
        } else {
            level_label_->set_text(
                "Peak: " + format_dbfs(snapshot_.peak_max) + " dBFS\n" +
                "RMS: " + format_dbfs(snapshot_.rms_max) + " dBFS");
        }
    }

    if (content_label_) {
        if (status_ == Status::kNoProbe) {
            content_label_->set_text("Clip: —   NaN/Inf: —\nSilence: —");
        } else {
            content_label_->set_text(
                "Clip: " + std::to_string(snapshot_.clip_count) +
                "   NaN/Inf: " + std::to_string(snapshot_.nan_inf_count) + "\n" +
                "Silence run: " + std::to_string(snapshot_.silence_run_blocks) +
                " blocks");
        }
    }

    if (phase_label_) {
        if (status_ == Status::kNoProbe || snapshot_.channel_count < 2) {
            phase_label_->set_text("L/R match: —   Balance: —");
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "L/R match: %+.2f   Balance: %+.2f",
                          lr_match_, balance_);
            phase_label_->set_text(buf);
        }
    }

    if (device_label_) {
        if (status_ == Status::kNoProbe) {
            device_label_->set_text("Sample rate: —   Block: —\n"
                                    "Callbacks: —   xruns: —   overloads: —");
        } else {
            char buf[160];
            std::snprintf(
                buf, sizeof(buf),
                "Sample rate: %.0f Hz   Block: %u   Ch: %u\n"
                "Callbacks: %llu   xruns: %llu   overloads: %llu",
                snapshot_.sample_rate, snapshot_.block_size,
                snapshot_.channel_count,
                static_cast<unsigned long long>(snapshot_.callbacks),
                static_cast<unsigned long long>(stats_.device_xruns),
                static_cast<unsigned long long>(stats_.cpu_overloads));
            device_label_->set_text(buf);
        }
    }
}

}  // namespace pulp::view
