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
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/sample_key_map.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/offline_stretch.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/waveform_editor.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
    kRootNote     = 11, // MIDI note of slice 0 (slice idx = note - root)
    kOnsetSens    = 12, // 0..1 onset-detection sensitivity (higher = more slices)
};

// Editor waveform surface. Inherits WaveformEditor's waveform + slice-region
// rendering, and adds: a "drop a sample" call-to-action when empty, audio-file
// drop handling (load / replace), and a subtle teal flash on drop. It polls a
// generation counter every frame (set_continuous_repaint) so a drop decoded on
// the background worker refreshes the waveform without an explicit redraw call.
// Being a DropReceiver, the native drag dispatch (drag_drop.cpp) finds it via
// dynamic_cast at the hit-tested point.
class WaveformDropView : public view::WaveformEditor, public view::DropReceiver {
public:
    std::function<void(const std::string&)> on_file_dropped;
    std::function<void()> on_browse;             // click in the empty area -> file picker
    std::function<void(int slice_index, bool on)> on_play_slice;  // click a slice -> audition
    std::function<std::uint64_t()> generation;   // processor raw_generation()
    std::function<bool(std::vector<float>&, float&, std::vector<long>&)> snapshot;

    WaveformDropView() { set_continuous_repaint(true); }

    // Empty area: a left click opens the file picker (drag-drop backup).
    // Loaded: a left click on a slice auditions it (note = root + slice index),
    // instead of the base WaveformEditor's range selection.
    void on_mouse_event(const view::MouseEvent& event) override {
        const bool explicit_phase = event.hasExplicitPhase();
        const bool down = explicit_phase ? event.isPress() : event.is_down;
        const bool up = explicit_phase ? event.isRelease() : !event.is_down;
        if (!has_audio_) {
            if (down && event.button == view::MouseButton::left && on_browse) on_browse();
            return;
        }
        if (down && !pressed_ && event.button == view::MouseButton::left) {
            pressed_ = true;
            active_slice_ = slice_at_x(event.position.x);
            if (active_slice_ >= 0 && on_play_slice) on_play_slice(active_slice_, true);
        } else if (up && pressed_) {
            pressed_ = false;
            if (active_slice_ >= 0 && on_play_slice) on_play_slice(active_slice_, false);
            active_slice_ = -1;
        }
    }

    bool accept_drag(const view::DropData& d, view::Point) override {
        drag_over_ = first_audio_path(d).has_value();
        return drag_over_;
    }
    void leave_drag() override { drag_over_ = false; }
    bool accept_drop(const view::DropData& d, view::Point) override {
        drag_over_ = false;
        auto p = first_audio_path(d);
        if (!p) return false;
        if (on_file_dropped) on_file_dropped(*p);
        flash_.set(1.0f);
        flash_.animate_to(0.0f, 0.18f);  // ease-out fade (default easing)
        return true;
    }

    void paint(canvas::Canvas& c) override {
        refresh_if_dirty();
        advance_flash();
        view::WaveformEditor::paint(c);              // waveform + slice regions
        auto b = local_bounds();
        if (!has_audio_) paint_cta(c, b);
        if (drag_over_)  paint_drag_overlay(c, b);
        const float fa = std::clamp(flash_.value(), 0.0f, 1.0f);
        if (fa > 0.001f) {
            c.set_fill_color(canvas::Color::rgba8(
                0x16, 0xDA, 0xC2, static_cast<std::uint8_t>(0x55 * fa)));
            c.fill_rounded_rect(b.x, b.y, b.width, b.height, 6.0f);
        }
    }

private:
    static bool is_audio_ext(const std::string& path) {
        static const char* kExts[] = {".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg"};
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        for (const char* e : kExts) {
            const std::string es = e;
            if (lower.size() >= es.size() &&
                lower.compare(lower.size() - es.size(), es.size(), es) == 0)
                return true;
        }
        return false;
    }
    static std::optional<std::string> first_audio_path(const view::DropData& d) {
        if (d.type != view::DropData::Type::files) return std::nullopt;
        for (const auto& p : d.file_paths)
            if (is_audio_ext(p)) return p;
        return std::nullopt;
    }
    void refresh_if_dirty() {
        if (!generation || !snapshot) return;
        const std::uint64_t g = generation();
        if (g == last_gen_) return;
        last_gen_ = g;
        std::vector<float> mono; float sr = 48000.0f; std::vector<long> slices;
        if (snapshot(mono, sr, slices) && !mono.empty()) {
            set_audio_data(mono.data(), static_cast<int>(mono.size()), sr);
            clear_regions();
            for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
                view::WaveformRegion r;
                r.start_sample = static_cast<int>(slices[i]);
                r.end_sample = static_cast<int>(slices[i + 1]);
                r.color = canvas::Color::rgba8(0x16, 0xDA, 0xC2, (i % 2) ? 0x3C : 0x1A);
                add_region(r);
            }
            slices_ = std::move(slices);
            total_samples_ = static_cast<long>(mono.size());
            has_audio_ = true;
        } else {
            slices_.clear();
            total_samples_ = 0;
            has_audio_ = false;
        }
    }
    // Map a local x to a slice index (root + index = MIDI note). -1 if none.
    int slice_at_x(float x) const {
        auto b = local_bounds();
        if (b.width <= 0 || total_samples_ <= 0 || slices_.size() < 2) return -1;
        const float frac = std::clamp((x - b.x) / b.width, 0.0f, 0.999f);
        const long sample = static_cast<long>(frac * static_cast<float>(total_samples_));
        for (std::size_t i = 0; i + 1 < slices_.size(); ++i)
            if (sample >= slices_[i] && sample < slices_[i + 1]) return static_cast<int>(i);
        return static_cast<int>(slices_.size()) - 2;  // clamp to last slice
    }
    void advance_flash() {
        const auto now = std::chrono::steady_clock::now();
        if (have_last_paint_) {
            const float dt = std::chrono::duration<float>(now - last_paint_).count();
            flash_.advance(std::min(dt, 0.1f));
        }
        last_paint_ = now;
        have_last_paint_ = true;
    }
    void paint_cta(canvas::Canvas& c, const view::Rect& b) {
        c.set_fill_color(canvas::Color::rgba8(0x0F, 0x12, 0x17));
        c.fill_rounded_rect(b.x, b.y, b.width, b.height, 8.0f);
        c.set_stroke_color(canvas::Color::rgba8(0x28, 0x30, 0x3C));
        c.set_line_width(2.0f);
        c.stroke_rounded_rect(b.x + 6, b.y + 6, b.width - 12, b.height - 12, 8.0f);
        const float cx = b.x + b.width / 2, cy = b.y + b.height / 2 - 16;
        c.set_stroke_color(canvas::Color::rgba8(0x16, 0xDA, 0xC2));
        c.set_line_width(2.0f);
        c.stroke_line(cx, cy + 18, cx, cy - 4);
        c.stroke_line(cx - 9, cy + 6, cx, cy - 4);
        c.stroke_line(cx + 9, cy + 6, cx, cy - 4);
        c.set_fill_color(canvas::Color::rgba8(0xD6, 0xDC, 0xE4));
        c.set_font("", 14.0f);
        c.set_text_align(canvas::TextAlign::center);
        c.fill_text("Drop a sample, or click to browse", b.x + b.width / 2, cy + 42);
        c.set_fill_color(canvas::Color::rgba8(0x64, 0x6D, 0x7A));
        c.set_font("", 11.0f);
        c.fill_text("WAV  AIFF  FLAC  MP3  OGG", b.x + b.width / 2, cy + 62);
        c.set_text_align(canvas::TextAlign::left);
    }
    void paint_drag_overlay(canvas::Canvas& c, const view::Rect& b) {
        c.set_fill_color(canvas::Color::rgba8(0x16, 0xDA, 0xC2, 0x22));
        c.fill_rounded_rect(b.x, b.y, b.width, b.height, 8.0f);
        c.set_stroke_color(canvas::Color::rgba8(0x16, 0xDA, 0xC2));
        c.set_line_width(2.0f);
        c.stroke_rounded_rect(b.x + 2, b.y + 2, b.width - 4, b.height - 4, 8.0f);
    }

    bool has_audio_ = false;
    bool drag_over_ = false;
    bool have_last_paint_ = false;
    bool pressed_ = false;
    int active_slice_ = -1;
    std::vector<long> slices_;
    long total_samples_ = 0;
    std::uint64_t last_gen_ = ~0ull;  // force first refresh
    view::ValueAnimation flash_{0.0f};
    std::chrono::steady_clock::time_point last_paint_{};
};

// A label that re-renders from a callback every frame (continuous repaint) —
// for live readouts like the slice count that change after a drop / re-slice.
class LiveText : public view::View {
public:
    std::function<std::string()> text;
    canvas::Color color = canvas::Color::rgba8(0x93, 0x9C, 0xA9);
    float font_size = 11.0f;
    std::string font_family;
    LiveText() { set_continuous_repaint(true); }
    void paint(canvas::Canvas& c) override {
        if (!text) return;
        auto b = local_bounds();
        c.set_fill_color(color);
        c.set_font(font_family, font_size);
        c.set_text_align(canvas::TextAlign::left);
        c.fill_text(text(), b.x, b.y + b.height * 0.72f);
        c.set_text_align(canvas::TextAlign::left);
    }
};

// Editor root that owns the widget<->parameter bindings and store listeners.
// They are destroyed (and their store listeners removed) before the View base
// destroys the child widgets they reference — so a closing editor never fires a
// listener against a freed widget.
class SamplerEditorRoot : public view::View {
public:
    std::vector<view::ParameterBinding> bindings;
    std::vector<state::ListenerToken> listeners;
    std::function<void(int slice_index, bool on)> on_play_slice;  // musical typing

    SamplerEditorRoot() { set_focusable(true); }

    // Musical typing: home row a s d f g h j k l -> slices 0..8 (root + index).
    // De-duped so key auto-repeat doesn't retrigger a held note.
    bool on_key_event(const view::KeyEvent& event) override {
        const int idx = slice_for_key(event.key);
        if (idx < 0) return false;
        if (event.is_down) {
            if (held_[static_cast<std::size_t>(idx)]) return true;  // auto-repeat
            held_[static_cast<std::size_t>(idx)] = true;
            if (on_play_slice) on_play_slice(idx, true);
        } else {
            held_[static_cast<std::size_t>(idx)] = false;
            if (on_play_slice) on_play_slice(idx, false);
        }
        return true;
    }

private:
    static int slice_for_key(view::KeyCode k) {
        switch (k) {
            case view::KeyCode::a: return 0;
            case view::KeyCode::s: return 1;
            case view::KeyCode::d: return 2;
            case view::KeyCode::f: return 3;
            case view::KeyCode::g: return 4;
            case view::KeyCode::h: return 5;
            case view::KeyCode::j: return 6;
            case view::KeyCode::k: return 7;
            case view::KeyCode::l: return 8;
            default: return -1;
        }
    }
    std::array<bool, 9> held_{};
};

class PulpTempoSamplerProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices = 8;
    static constexpr std::uint32_t kMaxSampleChannels = SamplerSampleStore::kMaxChannels;
    static constexpr std::uint32_t kMaxOutputChannels = 8;
    static constexpr int kDefaultRootNote = 60;  // C3 in C-2..C8 labeling (see note_name)
    static constexpr int kRootNoteMin = 0;       // C-2
    static constexpr int kRootNoteMax = 120;     // C8

    // Onset sensitivity s in [0,1] maps (inversely) to OnsetDetectionConfig
    // threshold_multiplier: higher sensitivity -> lower multiplier -> MORE slices.
    //   multiplier = kSensMulHi - s * (kSensMulHi - kSensMulLo)
    static constexpr double kSensMulLo = 0.8;   // most sensitive (most onsets)
    static constexpr double kSensMulHi = 3.0;   // least sensitive (fewest onsets)
    // Default sensitivity reproduces the detector's own default multiplier (1.5).
    static constexpr float kDefaultOnsetSensitivity =
        static_cast<float>((kSensMulHi - 1.5) / (kSensMulHi - kSensMulLo));
    static double onset_multiplier_for(double sensitivity) {
        const double s = std::clamp(sensitivity, 0.0, 1.0);
        return kSensMulHi - s * (kSensMulHi - kSensMulLo);
    }

    // MIDI note name in the user-requested C-2..C8 convention (C3 = 60). NOTE:
    // this deliberately differs from Pulp's MidiKeyboard, which labels 60 as C4
    // ((note/12)-1); the sampler dropdown follows the requested C-2..C8 range.
    static std::string note_name(int note) {
        static const char* kNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        const int n = ((note % 12) + 12) % 12;
        const int octave = (note / 12) - 2;
        return std::string(kNames[n]) + std::to_string(octave);
    }

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
        store.add_parameter({.id = kRootNote, .name = "Root Note", .unit = "",
                             .range = {static_cast<float>(kRootNoteMin),
                                       static_cast<float>(kRootNoteMax),
                                       static_cast<float>(kDefaultRootNote), 1}});
        store.add_parameter({.id = kOnsetSens, .name = "Onset Sensitivity", .unit = "",
                             .range = {0, 1, kDefaultOnsetSensitivity, 0.01f}});
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
        raw_generation_.fetch_add(1, std::memory_order_acq_rel);
        request_render(pending_host_bpm_.load(std::memory_order_relaxed));
        return true;
    }

    /// Decode an audio file from disk and load it. For UI drag-drop / the
    /// background worker — NEVER call on the audio thread (it allocates + reads
    /// the filesystem). Replaces any currently-loaded loop.
    bool load_loop_from_path(const std::string& path) {
        auto data = audio::FormatRegistry::instance().read(path);
        if (!data || data->empty()) return false;
        std::vector<const float*> ch(data->num_channels());
        for (std::uint32_t c = 0; c < data->num_channels(); ++c)
            ch[c] = data->channels[c].data();
        return load_loop(ch.data(), static_cast<int>(data->num_channels()),
                         static_cast<long>(data->num_frames()), data->sample_rate);
    }

    /// UI drop handler -> worker: decode + load a file off the UI/audio threads.
    void request_load_path(const std::string& path) {
        { std::lock_guard<std::mutex> lock(drop_mutex_); pending_drop_path_ = path; }
        drop_flag_.store(true, std::memory_order_release);
    }
    /// UI -> worker: coalesced re-slice request (onset-sensitivity change).
    void request_reanalyze() { analysis_flag_.store(true, std::memory_order_release); }

    // ── UI-driven triggering (lock-free UI thread -> audio thread) ──────────
    // Lets the editor play slices without a hardware MIDI device — clicking a
    // slice or musical typing pushes notes here; process() drains them. Works
    // in both the standalone and a plugin host (UI is in-process).
    void ui_note_on(int note, float velocity = 0.8f) {
        ui_notes_.try_push(UiNote{note, velocity, true});
    }
    void ui_note_off(int note) { ui_notes_.try_push(UiNote{note, 0.0f, false}); }
    /// Play slice N: maps to MIDI note (root + N), same as a host note would.
    void play_slice(int index, bool on) {
        const int note = static_cast<int>(state().get_value(kRootNote)) + index;
        if (on) ui_note_on(note); else ui_note_off(note);
    }

    double detected_bpm() const { return loop_bpm_.load(std::memory_order_relaxed); }
    std::size_t num_slices() const {
        std::lock_guard<std::mutex> lock(raw_mutex_);
        return slices_orig_.empty() ? 0 : slices_orig_.size() - 1;
    }

    /// Monotonic counter bumped whenever the raw loop or its slices change.
    /// The editor view polls this to know when to refresh (cheap dirty-check).
    std::uint64_t raw_generation() const {
        return raw_generation_.load(std::memory_order_acquire);
    }
    /// UI-thread snapshot of the raw mono waveform + slice boundaries for the
    /// editor. Returns false when no sample is loaded. Copies under raw_mutex_.
    bool snapshot_for_view(std::vector<float>& mono_out, float& sr_out,
                           std::vector<long>& slices_out) const {
        std::lock_guard<std::mutex> lock(raw_mutex_);
        if (raw_frames_ <= 0) return false;
        mono_out.assign(raw_[0].begin(), raw_[0].begin() + raw_frames_);
        sr_out = static_cast<float>(raw_sr_);
        slices_out = slices_orig_;
        return true;
    }
    bool has_sample() const { return store_.has_sample(); }
    long published_frames() const { return static_cast<long>(store_.read_published_view().num_frames); }
    void set_loop_bpm_for_test(double bpm) { loop_bpm_.store(bpm, std::memory_order_relaxed); }
    /// Test-only: the slice index a MIDI note maps to under the current root
    /// (mirrors region_for_note's `idx = note - root`). Deterministic and
    /// independent of whether a sample is loaded.
    int slice_index_for_note_test(int note) const {
        return note - static_cast<int>(state().get_value(kRootNote));
    }

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

        auto root = std::make_unique<SamplerEditorRoot>();
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

        (void)panel; (void)raised; (void)text; (void)rect;

        // ── Header ── (mockup tabs/transport removed — only wired controls remain)
        label(20, 16, 360, 24, "PulpTempoSampler", textStr, 17, 600, LabelAlign::left);
        label(300, 20, 448, 18,
              "Tap a slice  ·  A S D F G H J K L play slices  ·  drop or click to load",
              faint, 11, 400, LabelAlign::right);

        // ── Waveform / drop area ── (enlarged now the mockup rows are gone).
        // Shows a "drop a sample" CTA when empty; the waveform + slice regions
        // when loaded. Click a slice (or musical-type) to audition it; drop or
        // click-to-browse loads/replaces. Polls the processor to refresh.
        auto wf = std::make_unique<WaveformDropView>();
        place(*wf, 20, 50, 720, 256);  // 20px margins both sides (760 - 2*20)
        wf->on_file_dropped = [this](const std::string& path) { request_load_path(path); };
        wf->on_browse = [this] {
            auto picked = platform::FileDialog::open_file(
                "Load a sample",
                {{"Audio Files", "wav;aif;aiff;flac;mp3;ogg"}}, "");
            if (picked && !picked->empty()) request_load_path(*picked);
        };
        wf->on_play_slice = [this](int idx, bool on) { play_slice(idx, on); };
        wf->generation = [this] { return raw_generation(); };
        wf->snapshot = [this](std::vector<float>& mono, float& sr, std::vector<long>& sl) {
            return snapshot_for_view(mono, sr, sl);
        };
        root->add_child(std::move(wf));

        // Musical typing (home row) routes to the same audition path.
        root->on_play_slice = [this](int idx, bool on) { play_slice(idx, on); };

        // ── Footer: wired controls only ──
        // Root-note dropdown (slice 0 maps to this MIDI note; idx = note - root).
        // Labels follow the requested C-2..C8 convention (C3 = 60); see note_name.
        label(20, 320, 40, 18, "ROOT", faint, 10, 600, LabelAlign::left, true);
        {
            auto combo = std::make_unique<ComboBox>();
            std::vector<std::string> names;
            names.reserve(static_cast<std::size_t>(kRootNoteMax - kRootNoteMin + 1));
            for (int n = kRootNoteMin; n <= kRootNoteMax; ++n) names.push_back(note_name(n));
            combo->set_items(std::move(names));
            const int cur = static_cast<int>(state().get_value(kRootNote));
            combo->set_selected_silent(std::clamp(cur - kRootNoteMin, 0, kRootNoteMax - kRootNoteMin));
            combo->on_change = [this](int idx) {
                const int note = kRootNoteMin + idx;
                state().begin_gesture(kRootNote);
                state().set_value(kRootNote, static_cast<float>(note));
                state().end_gesture(kRootNote);
            };
            ComboBox* comboPtr = combo.get();
            root->listeners.push_back(state().add_listener(
                [comboPtr](state::ParamID id, float v) {
                    if (id == kRootNote)
                        comboPtr->set_selected_silent(static_cast<int>(v) - kRootNoteMin);
                },
                state::ListenerThread::Main));
            place(*combo, 64, 316, 96, 26);
            root->add_child(std::move(combo));
        }

        // Onset-sensitivity fader: higher = more slices. Drag re-slices on the
        // worker (coalesced) via the kOnsetSens listener -> request_reanalyze().
        label(176, 320, 44, 18, "SENS", faint, 10, 600, LabelAlign::left, true);
        {
            auto fader = std::make_unique<Fader>();
            fader->set_orientation(Fader::Orientation::horizontal);
            place(*fader, 220, 322, 150, 16);
            root->bindings.push_back(bind_parameter(*fader, state(), kOnsetSens));
            root->add_child(std::move(fader));
            root->listeners.push_back(state().add_listener(
                [this](state::ParamID id, float) {
                    if (id == kOnsetSens) request_reanalyze();
                },
                state::ListenerThread::Main));
        }

        // Live slice count (updates after a drop or a sensitivity change).
        {
            auto live = std::make_unique<LiveText>();
            live->font_family = mono;
            live->text = [this] { return "SLICES  " + std::to_string(num_slices()); };
            place(*live, 390, 320, 140, 18);
            root->add_child(std::move(live));
        }

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

        // Drain UI-injected notes (slice clicks / musical typing) at block start.
        while (auto n = ui_notes_.try_pop()) {
            if (n->on) {
                if (can_trigger) trigger_note(n->note, n->velocity, published, params);
            } else {
                release_note(n->note);
            }
        }

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
        audio::OnsetDetectionConfig oc;
        oc.threshold_multiplier = onset_multiplier_for(state().get_value(kOnsetSens));
        const auto onsets = od.detect(view, oc);
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
                bool did_work = false;
                // Decode + load a dropped file off the UI/audio threads.
                if (drop_flag_.exchange(false, std::memory_order_acq_rel)) {
                    std::string path;
                    { std::lock_guard<std::mutex> lock(drop_mutex_); path = pending_drop_path_; }
                    if (!path.empty()) load_loop_from_path(path);
                    did_work = true;
                }
                // Onset re-analysis (e.g. sensitivity changed) coalesces to the
                // latest request, then triggers a re-render of the new slices.
                if (analysis_flag_.exchange(false, std::memory_order_acq_rel)) {
                    reanalyze();
                    did_work = true;
                }
                if (render_flag_.exchange(false, std::memory_order_acq_rel)) {
                    render_to_tempo(pending_host_bpm_.load(std::memory_order_relaxed));
                    did_work = true;
                }
                if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(3));
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
    // Worker: re-detect onsets with the current sensitivity, then re-render.
    void reanalyze() {
        {
            std::lock_guard<std::mutex> lock(raw_mutex_);
            if (raw_frames_ <= 0) return;
            analyze_locked();
        }
        raw_generation_.fetch_add(1, std::memory_order_acq_rel);
        request_render(pending_host_bpm_.load(std::memory_order_relaxed));
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
        const int root = static_cast<int>(state().get_value(kRootNote));
        {
            std::lock_guard<std::mutex> lock(slice_mutex_);
            if (slices_stretched_.size() >= 2) {
                const int idx = note - root;
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
    std::atomic<bool> analysis_flag_{false};
    std::atomic<bool> drop_flag_{false};
    std::atomic<std::uint64_t> raw_generation_{0};
    std::atomic<double> pending_host_bpm_{120.0};
    std::mutex drop_mutex_;
    std::string pending_drop_path_;

    // UI-thread -> audio-thread note injection (click/typing playback).
    struct UiNote { int note = -1; float velocity = 0.8f; bool on = false; };
    runtime::SpscQueue<UiNote, 256> ui_notes_;
};

inline std::unique_ptr<format::Processor> create_pulp_tempo_sampler() {
    return std::make_unique<PulpTempoSamplerProcessor>();
}

} // namespace pulp::examples
