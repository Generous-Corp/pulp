#pragma once

/// PulpTempoSampler — tempo-matching sampler. Load a loop; it detects the loop's
/// BPM and onsets, maps slices to MIDI notes, and plays them time-stretched to
/// the host tempo (Serato / Ableton-warp). Stretching is OFFLINE on a background
/// thread via pulp::signal::OfflineStretch; the audio thread only plays the
/// pre-rendered, generation-published buffer. Link (repitch/vinyl) vs unlink
/// (tempo-only), plus pitch + formant when unlinked.
///
/// Reuses PulpSampler's SamplerVoice + SamplerSampleStore + voice-render path.

#include <pulp/audio/built_in_key_tempo_analyzer.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/sample_key_map.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/offline_stretch.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/waveform_editor.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pulp::examples {

// Inlined from PulpSampler's sampler_components.hpp so this example is
// self-contained — pulp_add_plugin compiles the format entries without an
// extra cross-example include path.
struct SamplerVoice {
    bool active = false;
    int note = -1;
    float velocity = 0.0f;
    signal::Adsr adsr;
    audio::LoopRenderer renderer;
    audio::PublishedSampleView sample;
    bool released = false;

    void reset() {
        active = false; note = -1; velocity = 0.0f; sample = {}; released = false;
        adsr.reset(); renderer.reset();
    }
    bool start(int n, float vel, double speed, float host_sample_rate,
               const audio::PublishedSampleView& sample_view,
               const audio::LoopRegion& region, std::uint64_t source_frames) {
        reset();
        if (!renderer.set_region(region, source_frames)) return false;
        note = n; velocity = vel; sample = sample_view; active = true;
        adsr.set_sample_rate(host_sample_rate); adsr.note_on();
        renderer.set_playback_rate(speed); renderer.start();
        return true;
    }
    void release() { adsr.note_off(); released = true; }
};

class SamplerSampleStore : public audio::PublishedSampleStore {
public:
    static constexpr std::uint32_t kSlotCount = 2;
    static constexpr std::uint32_t kMaxChannels = 2;
    static constexpr std::uint64_t kMaxFrames = 48000ull * 60ull;
    bool prepare() {
        return audio::PublishedSampleStore::prepare(
            audio::PublishedSampleStoreConfig{kSlotCount, kMaxChannels, kMaxFrames});
    }
};

enum TempoSamplerParams : state::ParamID {
    kTempoGain    = 1,
    kTempoAttack  = 2,
    kTempoDecay   = 3,
    kTempoSustain = 4,
    kTempoRelease = 5,
    kTempoLink    = 6, // 0 = unlink (tempo-only), 1 = link (repitch/vinyl)
    kTempoPitch   = 7, // semitones (unlink only)
    kTempoFormant = 8, // 0 follow, 1 preserve, 2 independent
    kTempoQuality = 9, // 0 draft, 2 best
    kTempoLoop    = 10,
};

class PulpTempoSamplerProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices = 8;
    static constexpr std::uint32_t kMaxSampleChannels = SamplerSampleStore::kMaxChannels;
    static constexpr std::uint32_t kMaxOutputChannels = 8;
    static constexpr int kRootNote = 60;

    ~PulpTempoSamplerProcessor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpTempoSampler",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.tempo-sampler",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kTempoGain, .name = "Gain", .unit = "dB", .range = {-60, 12, 0, 0.1f}});
        store.add_parameter({.id = kTempoAttack, .name = "Attack", .unit = "ms", .range = {0, 5000, 1, 1}});
        store.add_parameter({.id = kTempoDecay, .name = "Decay", .unit = "ms", .range = {0, 5000, 50, 1}});
        store.add_parameter({.id = kTempoSustain, .name = "Sustain", .unit = "%", .range = {0, 100, 100, 1}});
        store.add_parameter({.id = kTempoRelease, .name = "Release", .unit = "ms", .range = {0, 10000, 50, 1}});
        store.add_parameter({.id = kTempoLink, .name = "Link", .unit = "", .range = {0, 1, 0, 1}});
        store.add_parameter({.id = kTempoPitch, .name = "Pitch", .unit = "st", .range = {-24, 24, 0, 1}});
        store.add_parameter({.id = kTempoFormant, .name = "Formant", .unit = "", .range = {0, 2, 1, 1}});
        store.add_parameter({.id = kTempoQuality, .name = "Quality", .unit = "", .range = {0, 2, 2, 1}});
        store.add_parameter({.id = kTempoLoop, .name = "Loop", .unit = "", .range = {0, 1, 1, 1}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        stop_worker();
        host_sample_rate_ = static_cast<float>(ctx.sample_rate);
        max_block_frames_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(ctx.max_buffer_size));
        prepared_output_channels_ = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(ctx.output_channels), 1, kMaxOutputChannels);
        for (std::uint32_t ch = 0; ch < kMaxOutputChannels; ++ch)
            voice_scratch_[ch].assign(max_block_frames_, 0.0f);
        store_.prepare();
        engine_.prepare(ctx.sample_rate, 2); // default [0.25x,4x] / ±24 st
        for (auto& voice : voices_) voice.reset();
        publish_audio_acknowledgement(store_.read_published_view());
        start_worker();
    }

    // ── Off-audio-thread API (host UI / drag-drop / tests) ─────────────────

    /// Load a loop (planar; 1 or 2 channels). Detects BPM + onsets and requests
    /// an initial render at the last-seen host tempo.
    bool load_loop(const float* const* channels, int num_channels, long frames, double sample_rate) {
        if (frames <= 0 || num_channels < 1) return false;
        std::lock_guard<std::mutex> lock(raw_mutex_);
        raw_channels_ = std::min(num_channels, 2);
        raw_frames_ = frames;
        raw_sr_ = sample_rate;
        for (int c = 0; c < 2; ++c) {
            raw_[c].assign(static_cast<size_t>(frames), 0.0f);
            const float* src = channels[std::min(c, raw_channels_ - 1)];
            std::copy(src, src + frames, raw_[c].begin());
        }
        analyze_locked();
        request_render(pending_host_bpm_.load(std::memory_order_relaxed));
        return true;
    }

    double detected_bpm() const { return loop_bpm_.load(std::memory_order_relaxed); }
    std::size_t num_slices() const {
        std::lock_guard<std::mutex> lock(raw_mutex_);
        return slices_orig_.empty() ? 0 : slices_orig_.size() - 1;
    }
    bool has_sample() const { return store_.has_sample(); }
    long published_frames() const { return static_cast<long>(store_.read_published_view().num_frames); }
    void set_loop_bpm_for_test(double bpm) { loop_bpm_.store(bpm, std::memory_order_relaxed); }

    /// Render synchronously (tests/headless). Real hosts use the worker.
    void render_now(double host_bpm) { render_to_tempo(host_bpm); }

    // ── Editor UI (Ink & Signal) ─────────────────────────────────────────
    /// The editor is an absolute-positioned 760×372 design; it does not
    /// reflow, so lock the host window to that size (min = max = preferred,
    /// aspect pinned) across all formats and the standalone GPU host.
    format::ViewSize view_size() const override {
        return {760, 372, 760, 372, 760, 372, 760.0 / 372.0};
    }

    /// Build the plugin editor: a graphite panel with a signal-teal waveform
    /// view of the loaded loop, slice regions shaded per onset. This is the
    /// Phase-5 waveform-editor pilot for the Ink & Signal design language.
    std::unique_ptr<view::View> create_view() override {
        using namespace pulp::view;
        using canvas::Color;

        // Ink & Signal palette.
        const Color bg900   = Color::rgba8(0x16, 0x1A, 0x20);
        const Color panel   = Color::rgba8(0x1E, 0x25, 0x30);
        const Color raised  = Color::rgba8(0x28, 0x30, 0x3C);
        const Color teal    = Color::rgba8(0x16, 0xDA, 0xC2);
        const Color tealSoft= Color::rgba8(0x16, 0xDA, 0xC2, 0x28);
        const Color textStr = Color::rgba8(0xF3, 0xF6, 0xF9);
        const Color text    = Color::rgba8(0xD6, 0xDC, 0xE4);
        const Color muted   = Color::rgba8(0x93, 0x9C, 0xA9);
        const Color faint   = Color::rgba8(0x64, 0x6D, 0x7A);
        const char* mono    = "JetBrains Mono";

        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, 760, 372});
        root->set_background_color(bg900);

        Theme theme = Theme::dark();
        theme.colors["waveform"]        = teal;
        theme.colors["waveform_bg"]     = Color::rgba8(0x0F, 0x12, 0x17);
        theme.colors["waveform_center"] = Color::rgba8(0x39, 0x41, 0x4A);
        theme.colors["playhead"]        = Color::rgba8(0x46, 0xF0, 0xDB);
        theme.colors["selection"]       = tealSoft;
        root->set_theme(theme);

        auto place = [](View& v, float x, float y, float w, float h) {
            v.set_position(View::Position::absolute);
            v.set_left(x); v.set_top(y);
            v.flex().dim_width = {w, DimensionUnit::px};
            v.flex().dim_height = {h, DimensionUnit::px};
            // yoga_layout.cpp applies an explicit px size from preferred_width /
            // preferred_height (resolve_dimensions normally fills these from
            // dim_*, but it doesn't run for absolute leaves on a bare
            // layout_children() pass — and the screenshot path re-runs layout
            // after create_view(), so a post-layout set_bounds() would be
            // discarded). Set them here so a leaf with no text measure func
            // (e.g. WaveformEditor) still gets a non-empty frame.
            v.flex().preferred_width = w;
            v.flex().preferred_height = h;
        };
        auto rect = [&](float x, float y, float w, float h, Color c) {
            auto v = std::make_unique<View>(); place(*v, x, y, w, h);
            v->set_background_color(c); root->add_child(std::move(v));
        };
        auto label = [&](float x, float y, float w, float h, std::string t, Color c,
                         float size, int weight, LabelAlign al, bool monofont = false) {
            auto l = std::make_unique<Label>(std::move(t)); place(*l, x, y, w, h);
            l->set_text_color(c); l->set_font_size(size); l->set_font_weight(weight);
            l->set_text_align(al);
            if (monofont) l->set_font_family(mono);
            root->add_child(std::move(l));
        };

        // ── Header ──
        label(20, 14, 320, 24, "Waveform editor", textStr, 17, 600, LabelAlign::left);
        label(182, 18, 280, 18, "trim  fades  slice  zoom  record", faint, 11, 400, LabelAlign::left);
        rect(560, 14, 54, 24, tealSoft);   label(560, 17, 54, 18, "4.20 S", teal, 11, 600, LabelAlign::center, true);
        rect(620, 14, 58, 24, raised);     label(620, 17, 58, 18, "96 KHZ", muted, 10, 600, LabelAlign::center, true);
        label(684, 17, 40, 18, "SNAP", faint, 10, 600, LabelAlign::left, true);
        label(720, 17, 40, 18, "ZERO-X", teal, 10, 600, LabelAlign::left, true);

        // ── Tab + transport row ──
        rect(20, 50, 260, 32, panel);
        rect(24, 54, 86, 24, raised);  label(24, 58, 86, 18, "One Shot", textStr, 12, 600, LabelAlign::center);
        label(110, 58, 78, 18, "Slice", muted, 12, 500, LabelAlign::center);
        label(188, 58, 88, 18, "Recorder", muted, 12, 500, LabelAlign::center);
        rect(516, 50, 78, 32, tealSoft); label(516, 58, 78, 18, "Loop", teal, 12, 600, LabelAlign::center);
        rect(600, 50, 56, 32, raised);   label(600, 58, 56, 18, "Fwd", text, 12, 500, LabelAlign::center);
        label(662, 58, 38, 18, "ZOOM", faint, 10, 600, LabelAlign::left, true);
        rect(702, 50, 26, 32, panel);    label(702, 55, 26, 22, "-", text, 16, 500, LabelAlign::center);
        rect(732, 50, 26, 32, panel);    label(732, 55, 26, 22, "+", text, 16, 500, LabelAlign::center);

        // ── Waveform ──
        auto wf = std::make_unique<WaveformEditor>();
        place(*wf, 20, 92, 738, 222);
        {
            std::lock_guard<std::mutex> lock(raw_mutex_);
            if (raw_frames_ > 0)
                wf->set_audio_data(raw_[0].data(), static_cast<int>(raw_frames_),
                                   static_cast<float>(raw_sr_));
            for (std::size_t i = 0; i + 1 < slices_orig_.size(); ++i) {
                WaveformRegion r;
                r.start_sample = static_cast<int>(slices_orig_[i]);
                r.end_sample = static_cast<int>(slices_orig_[i + 1]);
                r.color = Color::rgba8(0x16, 0xDA, 0xC2, (i % 2) ? 0x3C : 0x1A);
                wf->add_region(r);
            }
        }
        root->add_child(std::move(wf));

        // ── Footer readout ──
        const std::size_t slices = num_slices();
        label(20, 326, 120, 18, "IN  0:00.00", text, 11, 500, LabelAlign::left, true);
        label(150, 326, 130, 18, "OUT  0:02.00", text, 11, 500, LabelAlign::left, true);
        label(290, 326, 150, 18, "FADE  12 / 80 MS", muted, 11, 500, LabelAlign::left, true);
        label(440, 326, 120, 18, "SLICES  " + std::to_string(slices), muted, 11, 500, LabelAlign::left, true);
        rect(596, 320, 60, 28, panel);   label(596, 325, 60, 18, "Snap", text, 12, 500, LabelAlign::center);
        rect(662, 320, 96, 28, tealSoft); label(662, 325, 96, 18, "Auto-slice", teal, 12, 600, LabelAlign::center);

        root->layout_children();
        return root;
    }

    // ── Audio thread ───────────────────────────────────────────────────────

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        clear_output(output);

        // Track host tempo; on change, ask the worker to re-render (RT-safe:
        // just atomic stores + flag, no allocation/locks on the audio thread).
        const double bpm = ctx.tempo_bpm > 0.0 ? ctx.tempo_bpm : 120.0;
        if (ctx.tempo_changed || bpm != last_host_bpm_) {
            last_host_bpm_ = bpm;
            request_render(bpm);
        }

        const auto published = store_.read_published_view();
        const bool can_trigger = store_.slot_view_valid(published);
        const auto params = current_params();
        const auto block_frames = static_cast<std::uint32_t>(output.num_samples());
        midi_in.sort();

        std::uint32_t cursor = 0;
        for (std::size_t i = 0; i < midi_in.size(); ++i) {
            const auto& event = midi_in[i];
            const auto offset = static_cast<std::uint32_t>(
                std::clamp(event.sample_offset, 0, static_cast<int32_t>(block_frames)));
            if (offset > cursor) render_active_voices(output, cursor, offset - cursor, params);
            if (event.message.isNoteOn() && can_trigger)
                trigger_note(event.message.getNoteNumber(),
                             static_cast<float>(event.message.getVelocity()) / 127.0f,
                             published, params);
            else if (event.message.isNoteOff())
                release_note(event.message.getNoteNumber());
            cursor = offset;
        }
        if (cursor < block_frames) render_active_voices(output, cursor, block_frames - cursor, params);
        publish_audio_acknowledgement(published);
    }

private:
    struct RenderParams {
        float gain = 1.0f;
        signal::Adsr::Params adsr;
        bool loop = false;
    };

    // ── Analysis (under raw_mutex_) ──
    void analyze_locked() {
        std::array<const float*, 2> ptrs{raw_[0].data(), raw_[1].data()};
        audio::BufferView<const float> view(ptrs.data(), static_cast<std::size_t>(raw_channels_),
                                            static_cast<std::size_t>(raw_frames_));
        audio::BuiltInKeyTempoAnalyzer kt;
        audio::KeyTempoAnalysisConfig kc;
        kc.source_sample_rate = raw_sr_;
        kc.channels = static_cast<std::uint32_t>(raw_channels_);
        const auto kr = kt.analyze(view, kc);
        loop_bpm_.store(kr.tempo_bpm, std::memory_order_relaxed);

        audio::OnsetDetector od;
        const auto onsets = od.detect(view);
        slices_orig_.clear();
        slices_orig_.push_back(0);
        for (const auto& m : onsets.markers)
            if (static_cast<long>(m.frame) > 0 && static_cast<long>(m.frame) < raw_frames_)
                slices_orig_.push_back(static_cast<long>(m.frame));
        slices_orig_.push_back(raw_frames_);
        std::sort(slices_orig_.begin(), slices_orig_.end());
        slices_orig_.erase(std::unique(slices_orig_.begin(), slices_orig_.end()), slices_orig_.end());
    }

    // ── Offline render to host tempo (worker thread) ──
    void render_to_tempo(double host_bpm) {
        std::vector<std::vector<float>> raw_copy;
        long frames; int ch; double loop_bpm;
        std::vector<long> slices;
        {
            std::lock_guard<std::mutex> lock(raw_mutex_);
            if (raw_frames_ <= 0) return;
            frames = raw_frames_; ch = raw_channels_;
            loop_bpm = loop_bpm_.load(std::memory_order_relaxed);
            raw_copy.assign(static_cast<size_t>(ch), {});
            for (int c = 0; c < ch; ++c) raw_copy[static_cast<size_t>(c)] = raw_[c];
            slices = slices_orig_;
        }

        // Duration scales as loop_bpm / host_bpm (faster host => shorter loop).
        double R = (loop_bpm > 0.0 && host_bpm > 0.0) ? (loop_bpm / host_bpm) : 1.0;
        R = std::clamp(R, 1.0 / engine_.max_time_ratio(), engine_.max_time_ratio());

        const bool link = state().get_value(kTempoLink) >= 0.5f;
        signal::OfflineStretchOptions o;
        o.time_ratio = R;
        o.repitch_linked = link;
        if (!link) {
            o.pitch_semitones = std::clamp<double>(state().get_value(kTempoPitch),
                                                   -engine_.max_pitch_semitones(),
                                                   engine_.max_pitch_semitones());
            const int fm = static_cast<int>(state().get_value(kTempoFormant) + 0.5f);
            o.formant_mode = (fm <= 0) ? signal::OfflineFormantMode::follow_pitch
                            : (fm == 1) ? signal::OfflineFormantMode::preserve_original
                                        : signal::OfflineFormantMode::shift_independently;
        }
        o.quality = static_cast<int>(state().get_value(kTempoQuality) + 0.5f);

        const long out_frames = signal::offline_stretch_output_frames(frames, R);
        if (out_frames <= 0) return;

        std::vector<std::vector<float>> stretched(static_cast<size_t>(ch),
                                                  std::vector<float>(static_cast<size_t>(out_frames)));
        std::vector<const float*> inp(static_cast<size_t>(ch));
        std::vector<float*> outp(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) { inp[c] = raw_copy[static_cast<size_t>(c)].data(); outp[c] = stretched[static_cast<size_t>(c)].data(); }

        signal::OfflineStretch local;          // worker-local engine instance
        local.prepare(static_cast<double>(host_sample_rate_), ch);
        std::string err;
        if (!local.process(inp.data(), frames, outp.data(), out_frames, o, &err)) return;

        // Publish the stretched buffer (generation-safe; in-flight voices keep
        // their old generation until they finish — RT lifetime via the store).
        const auto gen = audio_ack_generation_.load(std::memory_order_acquire);
        bool ok = false;
        if (ch == 1) {
            ok = store_.load_mono(stretched[0].data(), static_cast<int>(out_frames),
                                  host_sample_rate_, gen);
        } else {
            std::vector<float> inter(static_cast<size_t>(out_frames) * 2);
            for (long i = 0; i < out_frames; ++i) {
                inter[static_cast<size_t>(i) * 2] = stretched[0][static_cast<size_t>(i)];
                inter[static_cast<size_t>(i) * 2 + 1] = stretched[1][static_cast<size_t>(i)];
            }
            ok = store_.load_interleaved_stereo(inter.data(), static_cast<int>(out_frames),
                                                host_sample_rate_, gen);
        }
        if (!ok) return;

        // Scaled slice boundaries into the stretched buffer.
        std::vector<long> scaled;
        scaled.reserve(slices.size());
        for (long s : slices) scaled.push_back(std::min<long>(out_frames, static_cast<long>(std::llround(s * R))));
        {
            std::lock_guard<std::mutex> lock(slice_mutex_);
            slices_stretched_ = std::move(scaled);
        }
    }

    // ── Background worker ──
    void start_worker() {
        worker_run_.store(true, std::memory_order_release);
        worker_ = std::thread([this] {
            while (worker_run_.load(std::memory_order_acquire)) {
                if (render_flag_.exchange(false, std::memory_order_acq_rel)) {
                    render_to_tempo(pending_host_bpm_.load(std::memory_order_relaxed));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
        });
    }
    void stop_worker() {
        worker_run_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }
    void request_render(double bpm) {
        pending_host_bpm_.store(bpm, std::memory_order_relaxed);
        render_flag_.store(true, std::memory_order_release); // worker supersedes stale renders
    }

    RenderParams current_params() const {
        RenderParams p;
        p.gain = std::pow(10.0f, state().get_value(kTempoGain) / 20.0f);
        p.adsr.attack = state().get_value(kTempoAttack) / 1000.0f;
        p.adsr.decay = state().get_value(kTempoDecay) / 1000.0f;
        p.adsr.sustain = state().get_value(kTempoSustain) / 100.0f;
        p.adsr.release = state().get_value(kTempoRelease) / 1000.0f;
        p.loop = state().get_value(kTempoLoop) >= 0.5f;
        return p;
    }

    static void clear_output(audio::BufferView<float>& output) noexcept {
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
            std::fill_n(output.channel_ptr(ch), output.num_samples(), 0.0f);
    }

    // Region for a note: a slice of the published (already tempo-matched) buffer.
    audio::LoopRegion region_for_note(int note, const audio::PublishedSampleView& sample,
                                      bool loop) const noexcept {
        std::uint64_t start = 0, end = sample.num_frames;
        {
            std::lock_guard<std::mutex> lock(slice_mutex_);
            if (slices_stretched_.size() >= 2) {
                const int idx = note - kRootNote;
                if (idx >= 0 && idx + 1 < static_cast<int>(slices_stretched_.size())) {
                    start = static_cast<std::uint64_t>(slices_stretched_[static_cast<size_t>(idx)]);
                    end = static_cast<std::uint64_t>(slices_stretched_[static_cast<size_t>(idx) + 1]);
                }
            }
        }
        if (end <= start || end > sample.num_frames) { start = 0; end = sample.num_frames; }
        audio::LoopRegion region;
        region.start_frame = start;
        region.end_frame = end;
        region.source_sample_rate = sample.sample_rate;
        region.playback_mode = loop ? audio::LoopPlaybackMode::Forward : audio::LoopPlaybackMode::OneShot;
        region.interpolation = audio::LoopInterpolationMode::Linear;
        region.crossfade_curve = audio::LoopCrossfadeCurve::Linear;
        return region;
    }

    void render_active_voices(audio::BufferView<float>& output, std::uint32_t start_frame,
                              std::uint32_t frames, const RenderParams& params) noexcept {
        if (frames == 0) return;
        const auto out_ch = std::min<std::uint32_t>(
            {static_cast<std::uint32_t>(output.num_channels()), prepared_output_channels_, kMaxOutputChannels});
        if (out_ch == 0) return;
        for (std::uint32_t ch = 0; ch < out_ch; ++ch) voice_scratch_ptrs_[ch] = voice_scratch_[ch].data();

        std::uint32_t rendered = 0;
        while (rendered < frames) {
            const auto chunk = std::min(frames - rendered, max_block_frames_);
            audio::BufferView<float> scratch(voice_scratch_ptrs_.data(), out_ch, chunk);
            for (auto& voice : voices_) {
                if (!voice.active) continue;
                std::array<const float*, kMaxSampleChannels> sptrs{};
                if (!store_.populate_channel_ptrs(voice.sample, sptrs.data(), sptrs.size())) { voice.reset(); continue; }
                audio::BufferView<const float> source(sptrs.data(), voice.sample.num_channels,
                                                      static_cast<std::size_t>(voice.sample.num_frames));
                voice.adsr.set_params(params.adsr);
                const auto loop_result = voice.renderer.render(source, scratch, chunk);
                bool finished = false;
                for (std::uint32_t i = 0; i < chunk; ++i) {
                    const float env = voice.adsr.next();
                    if (env <= 0.0001f && voice.released) { finished = true; break; }
                    const float scale = env * voice.velocity * params.gain;
                    for (std::uint32_t ch = 0; ch < out_ch; ++ch)
                        output.channel_ptr(ch)[start_frame + rendered + i] += voice_scratch_[ch][i] * scale;
                }
                if (finished || !loop_result.active) voice.reset();
            }
            rendered += chunk;
        }
    }

    void trigger_note(int note, float velocity, const audio::PublishedSampleView& sample,
                      const RenderParams& params) {
        SamplerVoice* target = nullptr;
        for (auto& voice : voices_) if (!voice.active) { target = &voice; break; }
        if (target == nullptr) target = &voices_[0];
        const auto region = region_for_note(note, sample, params.loop);
        // Buffer is already at host tempo -> play at native rate (1.0).
        target->start(note, velocity, 1.0, host_sample_rate_, sample, region, sample.num_frames);
    }

    void release_note(int note) {
        for (auto& voice : voices_)
            if (voice.active && voice.note == note && !voice.released) voice.release();
    }

    std::uint64_t audio_safe_generation(const audio::PublishedSampleView& published) const noexcept {
        std::array<audio::PublishedSampleView, kMaxVoices> active{};
        std::size_t count = 0;
        for (const auto& v : voices_) if (v.active && v.sample.valid) active[count++] = v.sample;
        return audio::SampleSlotBank::oldest_active_generation(published, active.data(), count);
    }
    void publish_audio_acknowledgement(const audio::PublishedSampleView& published) noexcept {
        audio_ack_generation_.store(audio_safe_generation(published), std::memory_order_release);
    }

    // State
    SamplerSampleStore store_;
    signal::OfflineStretch engine_; // sizing reference (bounds); renders use a local instance
    std::array<std::vector<float>, kMaxOutputChannels> voice_scratch_{};
    std::array<float*, kMaxOutputChannels> voice_scratch_ptrs_{};
    SamplerVoice voices_[kMaxVoices]{};
    std::atomic<std::uint64_t> audio_ack_generation_{0};
    float host_sample_rate_ = 48000.0f;
    std::uint32_t max_block_frames_ = 512;
    std::uint32_t prepared_output_channels_ = 2;
    double last_host_bpm_ = 0.0;

    // Raw loop + analysis (guarded by raw_mutex_)
    mutable std::mutex raw_mutex_;
    std::vector<float> raw_[2];
    int raw_channels_ = 1;
    long raw_frames_ = 0;
    double raw_sr_ = 48000.0;
    std::atomic<double> loop_bpm_{0.0};
    std::vector<long> slices_orig_;

    mutable std::mutex slice_mutex_;
    std::vector<long> slices_stretched_;

    // Background render worker
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    std::atomic<bool> render_flag_{false};
    std::atomic<double> pending_host_bpm_{120.0};
};

inline std::unique_ptr<format::Processor> create_pulp_tempo_sampler() {
    return std::make_unique<PulpTempoSamplerProcessor>();
}

} // namespace pulp::examples
