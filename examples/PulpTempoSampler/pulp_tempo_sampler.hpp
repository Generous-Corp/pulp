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
#include <pulp/format/standalone_settings.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/offline_stretch.hpp>
#include <pulp/signal/resampler.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/musical_typing.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/waveform_editor.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace pulp::examples {

inline constexpr const char* kPulpTempoSamplerVersion = "1.2.0";

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
    audio::LoopRegion region;   // stored so the UI playhead can map position->progress
    bool released = false;
    bool sustained = false;     // note-off arrived but held by the sustain pedal

    void reset() {
        active = false; note = -1; velocity = 0.0f; sample = {}; released = false;
        sustained = false;
        adsr.reset(); renderer.reset();
    }
    bool start(int n, float vel, double speed, float host_sample_rate,
               const audio::PublishedSampleView& sample_view,
               const audio::LoopRegion& region_in, std::uint64_t source_frames) {
        reset();
        if (!renderer.set_region(region_in, source_frames)) return false;
        region = region_in;
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
    // Per-slot frame capacity. A sample whose STRETCHED length exceeds this can't
    // be published, so its slices map past the buffer and trigger silence — the
    // longer-sample-won't-play bug. 60 s was too small for full-length sources;
    // 4 min covers typical loops/songs. render_to_tempo additionally clamps the
    // publish to this cap so anything longer degrades gracefully (the head plays)
    // instead of silent-failing. (~184 MB preallocated across 2 stereo slots.)
    static constexpr std::uint64_t kMaxFrames = 48000ull * 240ull;
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
    kPlayToEnd    = 13, // Logic Quick Sampler "Play to End": a triggered slice
                        // plays through to the loop end (root => whole loop) so
                        // chopping (SENS>0) never steals whole-loop looping.
    kLoopMode     = 14, // loop DIRECTION when kTempoLoop is on: 0 Forward,
                        // 1 Reverse, 2 Ping-Pong. Surfaced (with One-Shot) as the
                        // MODE dropdown, which also drives kTempoLoop on/off.
    kSliceMode    = 15, // how cut points are chosen (Logic Slice "Mode" pop-up):
                        // 0 Onset/Transient (SENS=count), 1 Beat divisions
                        // (SENS=division 1/2/4/8/16), 2 Equal divisions (SENS=count).
    kGate         = 16, // 1 = key-up releases the envelope (default); 0 = One-Shot:
                        // a triggered slice plays through, ignoring note-off.
    kFlexSpeed    = 17, // Logic Flex Speed: index into {1/4, 1/2, 1, 2, 4}x of the
                        // tempo-synced playback speed. Default index 2 (1x).
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
    std::function<void(int&, float&)> playhead_query;             // (slice, 0..1 progress)
    std::function<std::uint64_t()> generation;   // processor raw_generation()
    std::function<bool(std::vector<float>&, float&, std::vector<long>&)> snapshot;

    WaveformDropView() { set_continuous_repaint(true); }

    // Empty area: a left click opens the file picker (drag-drop backup).
    // Loaded: a left click on a slice auditions it (note = root + slice index),
    // instead of the base WaveformEditor's range selection.
    void on_mouse_event(const view::MouseEvent& event) override {
        // Scroll/pinch INSIDE the waveform zooms (vertical, around the cursor)
        // and pans (horizontal) — no zoom buttons.
        if (event.is_wheel) { if (has_audio_) handle_wheel(event); return; }

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
        // Single playhead at the most-recently-triggered slice's position
        // (viewport-aware via sample_to_x, so it tracks correctly when zoomed).
        if (has_audio_ && playhead_query && total_samples_ > 0) {
            int s = -1; float prog = 0.0f;
            playhead_query(s, prog);
            if (s >= 0 && s + 1 < static_cast<int>(slices_.size())) {
                const double samp = static_cast<double>(slices_[static_cast<std::size_t>(s)]) +
                    static_cast<double>(prog) *
                    static_cast<double>(slices_[static_cast<std::size_t>(s) + 1] -
                                        slices_[static_cast<std::size_t>(s)]);
                const float x = sample_to_x(static_cast<int>(samp), b);
                if (x >= b.x && x <= b.x + b.width) {  // only when in view
                    c.set_stroke_color(canvas::Color::rgba8(0x46, 0xF0, 0xDB));
                    c.set_line_width(2.0f);
                    c.stroke_line(x, b.y, x, b.y + b.height);
                }
            }
        }
        const float fa = std::clamp(flash_.value(), 0.0f, 1.0f);
        if (fa > 0.001f) {
            c.set_fill_color(canvas::Color::rgba8(
                0x16, 0xDA, 0xC2, static_cast<std::uint8_t>(0x55 * fa)));
            c.fill_rounded_rect(b.x, b.y, b.width, b.height, 6.0f);
        }
    }

private:
    static bool is_audio_ext(const std::string& path) {
        // Accept exactly what the FormatRegistry can DECODE on this platform —
        // including .m4a / .aac / .alac / .caf via the macOS CoreAudio reader.
        // Sourcing the allow-list from the registry keeps drag-drop, the Open
        // dialog, and the decoders from drifting apart (the m4a-won't-open bug:
        // the decoder supported it but a hardcoded list here rejected the drop).
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        for (const auto& es : audio::FormatRegistry::instance().supported_read_extensions()) {
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
            set_visible_range(0, static_cast<int>(total_samples_));  // zoom-to-fit new sample
            has_audio_ = true;
        } else {
            slices_.clear();
            total_samples_ = 0;
            has_audio_ = false;
        }
    }
    // Scroll wheel / two-finger: vertical -> zoom around the cursor, horizontal
    // -> pan. Cursor-centred zoom keeps the sample under the pointer fixed.
    void handle_wheel(const view::MouseEvent& e) {
        auto b = local_bounds();
        if (b.width <= 0 || total_samples_ <= 0) return;
        const int total = static_cast<int>(total_samples_);
        const int len = visible_length() > 0 ? visible_length() : total;
        if (std::abs(e.scroll_delta_y) >= std::abs(e.scroll_delta_x)) {
            const float factor = std::exp(e.scroll_delta_y * 0.15f);  // scroll up -> zoom in
            int new_len = std::clamp(static_cast<int>(static_cast<float>(len) / factor), 64, total);
            const int s0 = x_to_sample(e.position.x, b);
            const float frac = std::clamp((e.position.x - b.x) / b.width, 0.0f, 1.0f);
            int new_start = s0 - static_cast<int>(frac * static_cast<float>(new_len));
            new_start = std::clamp(new_start, 0, std::max(0, total - new_len));
            set_visible_range(new_start, new_len);
        } else {
            scroll(static_cast<int>(e.scroll_delta_x * static_cast<float>(len) * 0.01f));
        }
    }

    // Map a local x to a slice index (root + index = MIDI note). -1 if none.
    // Viewport-aware (uses the zoom/scroll transform) so clicks land on the
    // right slice even when zoomed in.
    int slice_at_x(float x) const {
        if (total_samples_ <= 0 || slices_.size() < 2) return -1;
        const long sample = static_cast<long>(x_to_sample(x, local_bounds()));
        for (std::size_t i = 0; i + 1 < slices_.size(); ++i)
            if (sample >= slices_[i] && sample < slices_[i + 1]) return static_cast<int>(i);
        if (sample >= slices_.back()) return static_cast<int>(slices_.size()) - 2;
        return 0;
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
        c.fill_text("WAV  AIFF  FLAC  MP3  OGG  M4A", b.x + b.width / 2, cy + 62);
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

// Invisible zero-chrome view that runs a callback once per frame (continuous
// repaint). Used to poll the processor's loop generation so the footer TEMPO
// box can default to the freshly-detected loop BPM (R≈1) when a new loop loads.
class FrameTick : public view::View {
public:
    std::function<void()> on_tick;
    FrameTick() { set_continuous_repaint(true); }
    void paint(canvas::Canvas&) override { if (on_tick) on_tick(); }
};

// Editor root that owns the widget<->parameter bindings and store listeners.
// They are destroyed (and their store listeners removed) before the View base
// destroys the child widgets they reference — so a closing editor never fires a
// listener against a freed widget.
class SamplerEditorRoot : public view::View {
public:
    std::vector<view::ParameterBinding> bindings;
    std::vector<state::ListenerToken> listeners;
    // Musical typing (computer keyboard -> notes -> slices). The SDK controller
    // owns the QWERTY->note map, dedup, modifier-chord rejection and note-off;
    // create_view() wires its on_note_on/off to the processor's RT note queue and
    // current_root_note to the ROOT param.
    view::MusicalTypingController typing;
    std::function<int()> current_root_note;
    // True while the SDK MusicalTypingKeyboard's own window is on-screen. The
    // editor's typing only plays slices (and lights the keyboard) while that
    // window is visible — when the keyboard is hidden, QWERTY triggers nothing.
    // This gates the MAIN-window typing path; when the keyboard's OWN window is
    // focused it self-captures keys instead (set_input_capture(true)), so the two
    // paths cover the two focus cases without overlapping. create_view() wires
    // this to `kb_window_ && kb_window_->is_visible()`; tests inject a bool.
    std::function<bool()> keyboard_window_visible;
    // Toggle the SDK MusicalTypingKeyboard's own secondary GPU window (Cmd-K /
    // Ctrl+K, and the toolbar "Keyboard" button). The keyboard is NOT inline —
    // the processor owns the window + primitive; create_view() wires this to
    // PulpTempoSamplerProcessor::toggle_keyboard_window().
    std::function<void()> on_toggle_keyboard;
    // Toolbar "Settings" button that opens the standalone Audio/MIDI panel. It is
    // only meaningful in the standalone (the SDK settings chrome owns the panel);
    // in a DAW the host owns device routing, so it is hidden. Visibility is
    // resolved once the view is attached (see update_standalone_chrome_affordances).
    view::TextButton* settings_button = nullptr;
    // Footer TEMPO box (target-tempo override) + its loop-generation watermark.
    // The FrameTick poller defaults the fader/readout to the detected loop BPM
    // whenever a new loop is analyzed (generation bump), so R≈1 on load.
    view::Fader* tempo_fader = nullptr;
    view::ToggleButton* link_btn = nullptr;  // tempo LINK toggle (follow host vs manual)
    view::View* detail_overlay_ = nullptr;   // Logic-style "Detail" page (Gate, Flex Speed, …)
    bool hosted_in_standalone_ = false;      // resolved on view-open
    std::uint64_t tempo_seen_gen_ = ~0ull;  // force a first sync

    // Reveal the "Settings" button only when hosted in the standalone settings
    // chrome (a TabPanel ancestor carrying the Audio/MIDI Settings tab). Called
    // from Processor::on_view_opened — by then the editor is parented into the
    // chrome, so format::standalone_settings_available() can detect it. Cheap and
    // allocation-free, so it is safe outside the paint no-alloc scope. Also records
    // the standalone-vs-DAW verdict so the processor can resolve LINKED tempo.
    void update_standalone_chrome_affordances() {
        hosted_in_standalone_ = format::standalone_settings_available(this);
        if (settings_button)
            settings_button->set_visible(hosted_in_standalone_);
    }

    SamplerEditorRoot() {
        set_focusable(true);
        // The waveform/keyboard editor renders through Skia, so request the GPU
        // PluginViewHost in plugin formats. Without this, decide_gpu_host() picks
        // the CPU CoreGraphics host (mode=autoui) and the Skia content renders
        // poorly/blank in a DAW — the standalone already uses the GPU WindowHost.
        set_requires_gpu_host(true);
        // Receive keys whether or not the editor holds focus: the host's
        // global-key hook fires on the window root every keystroke (standalone),
        // and on_key_event covers the focused path.
        on_global_key = [this](const view::KeyEvent& e) { return on_musical_key(e); };
    }

    bool on_key_event(const view::KeyEvent& event) override { return on_musical_key(event); }

    // Release any held typing notes (e.g. on focus loss) so nothing sticks.
    void release_all_typing() { typing.all_notes_off(); }

    // Toggle the MusicalTypingKeyboard window (same target as the toolbar button).
    void toggle_keyboard() { if (on_toggle_keyboard) on_toggle_keyboard(); }

private:
    bool on_musical_key(const view::KeyEvent& e) {
        // ⌘K (macOS) / Ctrl+K (Win/Linux): toggle the MusicalTypingKeyboard
        // window. Checked before the modifier-chord rejection below so the
        // command-combo reaches here (the macOS host routes Cmd chords via
        // performKeyEquivalent:).
        const uint16_t toggle_mod = view::kModCmd | view::kModCtrl;
        if (e.key == view::KeyCode::k && (e.modifiers & toggle_mod) && e.is_down && !e.is_repeat) {
            toggle_keyboard();
            return true;
        }
        // Don't turn typing into a name field into notes — if a text-input widget
        // owns focus, let it have the keys.
        if (auto* fv = view::View::focused_input_; fv && fv->accepts_text_input())
            return false;
        // Musical typing only PLAYS while the keyboard window is on-screen. A
        // key-DOWN with the keyboard hidden does not trigger a slice (returns
        // false so the host keeps the key). A key-UP always passes through so a
        // note that was held when the keyboard was hidden still gets released —
        // no stuck note. ⌘K above is unaffected: it toggles regardless.
        if (e.is_down && (!keyboard_window_visible || !keyboard_window_visible()))
            return false;
        if (current_root_note) typing.set_base_note(current_root_note());
        return typing.handle_key(e);  // false for unmapped / modifier chords -> host keeps them
    }
};

class PulpTempoSamplerProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices = 8;
    static constexpr std::uint32_t kMaxSampleChannels = SamplerSampleStore::kMaxChannels;
    static constexpr std::uint32_t kMaxOutputChannels = 8;
    static constexpr int kDefaultRootNote = 48;  // C2 in C-2..C8 labeling (see note_name) —
                                                 // aligns with the DAW Musical Typing default octave
    static constexpr int kRootNoteMin = 0;       // C-2
    static constexpr int kRootNoteMax = 120;     // C8

    // Onset sensitivity s in [0,1] maps to a slice COUNT (the strongest cuts by
    // confidence are kept): s=0 -> 1 slice (whole sample), s=1 -> kMaxSlices.
    static constexpr int kMaxSlices = 32;
    // Default to the WHOLE loop (1 slice): a dropped loop plays + LOOPs as a
    // single 2-bar region that tiles the bar grid. Raising SENS chops it into
    // slices for per-key triggering. (A multi-slice default made holding the root
    // loop only slice 0 — a quarter of the loop — which read as "the loop ends
    // early.")
    static constexpr float kDefaultOnsetSensitivity = 0.0f;  // whole loop by default

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
            .version = kPulpTempoSamplerVersion,
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
        // Default OFF (one-shot): a triggered slice plays once. The LOOP toggle
        // enables Forward looping (held until note-off) for sustained/textural use.
        store.add_parameter({.id = kTempoLoop, .name = "Loop", .unit = "", .range = {0, 1, 0, 1}});
        store.add_parameter({.id = kRootNote, .name = "Root Note", .unit = "",
                             .range = {static_cast<float>(kRootNoteMin),
                                       static_cast<float>(kRootNoteMax),
                                       static_cast<float>(kDefaultRootNote), 1}});
        store.add_parameter({.id = kOnsetSens, .name = "Onset Sensitivity", .unit = "",
                             .range = {0, 1, kDefaultOnsetSensitivity, 0.01f}});
        // Default ON: a triggered slice plays through to the loop end, so the root
        // always yields the whole loop and raising SLICES (SENS) never removes
        // whole-loop looping. OFF = strict per-slice chop (Logic Quick Sampler
        // "Play to End"). Persisted via StateStore; pre-kPlayToEnd states load
        // the default ON. No custom-state version bump needed.
        store.add_parameter({.id = kPlayToEnd, .name = "Play To End", .unit = "", .range = {0, 1, 1, 1}});
        // Loop direction (used only when kTempoLoop is on): 0 Forward, 1 Reverse,
        // 2 Ping-Pong. The MODE dropdown writes this alongside kTempoLoop.
        store.add_parameter({.id = kLoopMode, .name = "Loop Mode", .unit = "", .range = {0, 2, 0, 1}});
        // Slicing mode (Logic Slice "Mode" pop-up): 0 Onset, 1 Beat, 2 Equal.
        store.add_parameter({.id = kSliceMode, .name = "Slice Mode", .unit = "", .range = {0, 2, 0, 1}});
        // Gate: 1 = key-up releases the envelope (default), 0 = One-Shot.
        store.add_parameter({.id = kGate, .name = "Gate", .unit = "", .range = {0, 1, 1, 1}});
        // Flex Speed: index into {1/4,1/2,1,2,4}x; default 1x (index 2).
        store.add_parameter({.id = kFlexSpeed, .name = "Flex Speed", .unit = "", .range = {0, 4, 2, 1}});
    }

    // Logic Flex Speed factor: the loop plays this many times faster than the
    // tempo-synced base (so a higher factor SHORTENS the stretched duration).
    double flex_speed_factor() const noexcept {
        static constexpr double kSpeeds[] = {0.25, 0.5, 1.0, 2.0, 4.0};
        const int i = std::clamp(static_cast<int>(state().get_value(kFlexSpeed) + 0.5f), 0, 4);
        return kSpeeds[i];
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
        // Reactivation (sample-rate change / re-prepare) can arrive without the host
        // first sending note-offs. Clear the held-note set + edge-detect state so a
        // later LOOP-on can't resurrect a phantom chord from a stale held velocity.
        std::fill(std::begin(held_velocity_), std::end(held_velocity_), 0.0f);
        loop_prev_ = false;
        sustain_prev_ = false;
        sustain_pedal_.store(false, std::memory_order_relaxed);
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
        resample_raw_to_host_locked();
        analyze_locked();
        // A fresh drop while UNLINKED adopts the new sample's own (bar-snapped)
        // tempo, so the loop plays at its natural fractional speed; LINKED stays
        // following the host. deserialize re-applies a saved tempo AFTER load_loop,
        // so a restored manual tempo still wins over this.
        if (!tempo_linked()) {
            const double det = loop_bpm_.load(std::memory_order_relaxed);
            if (det > 0.0)
                tempo_override_.store(std::clamp(det, kTargetBpmMin, kTargetBpmMax),
                                      std::memory_order_relaxed);
        }
        raw_generation_.fetch_add(1, std::memory_order_acq_rel);
        load_generation_.fetch_add(1, std::memory_order_acq_rel);  // NEW sample → tempo poller re-detects BPM
        request_render(effective_bpm());  // render at the resolved tempo (host / detected / manual)
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

    // ── Plugin state: persist the loaded sample across reload / project save ──
    // The host calls these on save/restore; we serialize the raw loop audio so a
    // dropped sample survives closing+reopening the editor, plugin reload, and
    // DAW project save. (The slices/tempo re-derive on load.)
    std::vector<std::uint8_t> serialize_plugin_state() const override {
        std::lock_guard<std::mutex> lock(raw_mutex_);
        std::vector<std::uint8_t> out;
        if (raw_frames_ <= 0) return out;  // nothing loaded
        const std::uint32_t magic = 0x504C5053u;  // 'PLPS'
        const std::uint32_t version = 2;  // v2 appends the engaged target tempo
        const std::int32_t ch = raw_channels_;
        const std::int64_t frames = raw_frames_;
        const double sr = raw_sr_;
        auto put = [&out](const void* p, std::size_t n) {
            const auto* b = static_cast<const std::uint8_t*>(p);
            out.insert(out.end(), b, b + n);
        };
        put(&magic, 4); put(&version, 4); put(&ch, 4); put(&frames, 8); put(&sr, 8);
        for (int c = 0; c < raw_channels_; ++c)
            put(raw_[c].data(), static_cast<std::size_t>(raw_frames_) * sizeof(float));
        // v2: persist the TEMPO setting (0 = linked/follow-host, >0 = manual BPM) so
        // a chosen tempo survives close+reopen / project reload. Trails the audio so
        // a v1 reader simply ignores it.
        const double tempo = tempo_override_.load(std::memory_order_relaxed);
        put(&tempo, 8);
        return out;
    }
    bool deserialize_plugin_state(std::span<const std::uint8_t> data) override {
        constexpr std::size_t kHeader = 4 + 4 + 4 + 8 + 8;
        if (data.size() < kHeader) return true;  // no sample saved — fine
        const std::uint8_t* p = data.data();
        auto get = [&p](void* d, std::size_t n) { std::memcpy(d, p, n); p += n; };
        std::uint32_t magic = 0, version = 0; std::int32_t ch = 0;
        std::int64_t frames = 0; double sr = 0;
        get(&magic, 4); get(&version, 4); get(&ch, 4); get(&frames, 8); get(&sr, 8);
        if (magic != 0x504C5053u || frames <= 0 || ch < 1 || ch > 2 || sr <= 0) return true;
        const std::size_t need = kHeader + static_cast<std::size_t>(ch) *
                                 static_cast<std::size_t>(frames) * sizeof(float);
        if (data.size() < need) return false;
        std::vector<std::vector<float>> tmp(static_cast<std::size_t>(ch));
        std::vector<const float*> ptrs(static_cast<std::size_t>(ch));
        for (int c = 0; c < ch; ++c) {
            tmp[static_cast<std::size_t>(c)].resize(static_cast<std::size_t>(frames));
            std::memcpy(tmp[static_cast<std::size_t>(c)].data(), p,
                        static_cast<std::size_t>(frames) * sizeof(float));
            p += static_cast<std::size_t>(frames) * sizeof(float);
            ptrs[static_cast<std::size_t>(c)] = tmp[static_cast<std::size_t>(c)].data();
        }
        // v2: an engaged target tempo trails the audio. Read it BEFORE load_loop
        // (which resets generations but not the override), then apply it so the
        // saved tempo wins over the load's default-to-host render.
        bool have_tempo = false; double saved_tempo = 0.0;
        if (version >= 2 && data.size() >= need + 8) {
            std::memcpy(&saved_tempo, p, 8);
            have_tempo = std::isfinite(saved_tempo) && saved_tempo >= 0.0;
        }
        load_loop(ptrs.data(), ch, static_cast<long>(frames), sr);
        if (have_tempo) {
            if (saved_tempo > 0.0) set_target_bpm(saved_tempo);  // manual: re-engage + render
            else set_tempo_linked(true);                         // linked: follow host
        }
        return true;
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
    /// UI playhead: the most-recent active slice (-1 = none) + its 0..1 progress.
    void playhead(int& slice_out, float& pos_out) const {
        slice_out = ui_play_slice_.load(std::memory_order_acquire);
        pos_out = ui_play_pos_.load(std::memory_order_relaxed);
    }

    // ── Musical-typing keyboard window (SDK primitive) ──────────────────────
    // The keyboard's note sink: a MusicalTypingKeyboard emits ABSOLUTE MIDI
    // notes (base = ROOT + octave + semitone). They go to the SAME lock-free
    // UI→audio queue host MIDI / slice clicks use; region_for_note maps each to
    // slice idx = note - root.
    void sampler_note_on(int note, float velocity) { ui_note_on(note, velocity); }
    void sampler_note_off(int note) { ui_note_off(note); }

    // ── Single QWERTY/click source for the on-screen keyboard (UI thread) ────
    // Both the editor's MusicalTypingController (computer-keyboard typing, gated
    // on the keyboard window being visible) AND clicks on the keyboard's keys
    // route here. Each call plays/stops the slice via the lock-free UI→audio
    // queue AND maintains a set of currently-held MIDI notes that is pushed to
    // the keyboard (set_active_notes) so the matching keys light/unlight live.
    // When the MAIN editor window is focused this held set is the source of truth
    // for the keyboard's lit keys; when the keyboard's OWN window is focused it
    // self-captures keys (set_input_capture(true)) and routes them here too. Host MIDI
    // arrives on the audio thread and is not reflected here (no UI-thread hook).
    // Map a chromatic TYPING note to a slice-consecutive trigger note, so the
    // The typing keyboard is a chromatic piano (Logic-style Musical Typing): the
    // home row a,s,d,f,g,h,j,k plays the WHITE notes and the upper row w,e,t,y,u,o,p
    // plays the SHARPS above them (with the natural gaps at E/F and B/C). Each key is
    // a distinct semitone, so a slice-per-semitone mapping gives every key a UNIQUE
    // consecutive slice (a=slice0, w=slice1, s=slice2, e=slice3, ...) — identical to
    // host MIDI and waveform clicks, no remap. The physical note IS the trigger note,
    // so note-off always releases the voice it started even if ROOT changes mid-hold;
    // the held set still records each (physical, trigger) pair to keep that contract
    // explicit and to light the physical key.
    bool is_keyboard_note_held(int physical_note) const {
        return std::any_of(held_keyboard_triggers_ui_.begin(), held_keyboard_triggers_ui_.end(),
                           [physical_note](const HeldKeyboardTrigger& held) {
                               return held.physical_note == physical_note;
                           });
    }
    int held_trigger_note_for(int physical_note) const {
        for (const auto& held : held_keyboard_triggers_ui_)
            if (held.physical_note == physical_note) return held.trigger_note;
        return physical_note;  // chromatic 1:1 — the physical note is the trigger note
    }
    void forget_held_keyboard_trigger(int physical_note) {
        held_keyboard_triggers_ui_.erase(
            std::remove_if(held_keyboard_triggers_ui_.begin(), held_keyboard_triggers_ui_.end(),
                           [physical_note](const HeldKeyboardTrigger& held) {
                               return held.physical_note == physical_note;
                           }),
            held_keyboard_triggers_ui_.end());
    }
    std::vector<int> held_physical_notes() const {
        std::vector<int> notes;
        notes.reserve(held_keyboard_triggers_ui_.size());
        for (const auto& held : held_keyboard_triggers_ui_)
            notes.push_back(held.physical_note);
        return notes;
    }
    void keyboard_play_on(int note, float velocity) {
        ui_note_on(note, velocity);                      // slice per semitone, unique per key
        if (!is_keyboard_note_held(note))
            held_keyboard_triggers_ui_.push_back({note, note});
        push_keyboard_lights();
    }
    void keyboard_play_off(int note) {
        ui_note_off(held_trigger_note_for(note));
        forget_held_keyboard_trigger(note);
        push_keyboard_lights();
    }
    /// Push the current held-note set to the on-screen keyboard so it lights the
    /// matching keys (absolute MIDI). No-op until the keyboard window exists.
    void push_keyboard_lights() {
        if (kb_) {
            auto notes = held_physical_notes();
            kb_->set_active_notes(notes);
        }
    }
    /// Test-only: the held-note set currently lit on the on-screen keyboard.
    std::vector<int> held_notes_for_test() const { return held_physical_notes(); }
    /// Test-only: the TRIGGER note each currently-held key fires (the value passed to
    /// ui_note_on, i.e. the slice). Distinct from held_notes_for_test (physical keys)
    /// so a test can prove each key triggers a unique slice (no doubling).
    std::vector<int> held_trigger_notes_for_test() const {
        std::vector<int> notes;
        notes.reserve(held_keyboard_triggers_ui_.size());
        for (const auto& held : held_keyboard_triggers_ui_) notes.push_back(held.trigger_note);
        return notes;
    }
    /// Test-only: inject the SDK keyboard so the held-note → set_active_notes wiring
    /// can be exercised headlessly (the live path creates it in toggle_keyboard_window).
    void set_keyboard_for_test(std::unique_ptr<view::MusicalTypingKeyboard> kb) {
        kb_ = std::move(kb);
    }

    /// Toggle the MusicalTypingKeyboard's OWN secondary GPU window. Called from
    /// ⌘K and the toolbar "Keyboard" button (UI thread). Lazily creates the
    /// window + primitive on first open; subsequent calls show/hide it. The
    /// keyboard is the faithful Figma-SVG DesignFrameView, so the window MUST be
    /// GPU (use_gpu = true) — a CPU host renders it blank (mirrors mtk-demo).
    void toggle_keyboard_window() {
        if (kb_window_ && kb_window_->is_visible()) {
            // Hiding. Two things must happen so NO MIDI lingers after the keyboard
            // is dismissed:
            //  1) Drop the keyboard's self-capture — set_input_capture(false) fires
            //     on_note_off for every key the keyboard was holding (a key still
            //     physically down when the window hides never gets its own key-up,
            //     since a hidden window receives no keys), routing each through
            //     keyboard_play_off → ui_note_off. Without this the held note's
            //     voice sustained forever (the "QWERTY keeps sending MIDI after the
            //     keyboard is closed" bug).
            //  2) Belt-and-suspenders: release any note still in the UI-held set and
            //     clear the lit display so nothing lingers lit on the next open.
            kb_window_->set_app_key_monitor({});   // stop routing app keys to the keyboard
            if (kb_) kb_->set_input_capture(false);
            for (const auto& held : held_keyboard_triggers_ui_) ui_note_off(held.trigger_note);
            held_keyboard_triggers_ui_.clear();
            push_keyboard_lights();
            kb_window_->hide();
            return;
        }
        if (!kb_window_) {
            kb_ = std::make_unique<view::MusicalTypingKeyboard>();
            // The keyboard self-captures QWERTY (set_input_capture(true), the SDK
            // default + focusable). This matters because the keyboard lives in its
            // OWN secondary window: macOS routes typed keys to whichever window is
            // key, so when this window is focused, ONLY its root sees the keys —
            // the main editor's on_global_key does not fire (it's per-window). With
            // capture OFF the root was also non-focusable, so a focused keyboard
            // window dropped EVERY key (letters, numbers 1–8, tab) — dead typing.
            // Capture ON routes letters → on_note_on → keyboard_play_on (plays the
            // slice + lights via the held-note set) and numbers 1–8 / tab through
            // the SDK's control_press (pitch-bend / modulation / sustain buttons),
            // matching the click path. No double-trigger: only one window is key at
            // a time, so the editor's typing source (which plays + lights via
            // set_active_notes while the MAIN window is focused) and this keyboard
            // never both fire for the same keystroke.
            kb_->set_input_capture(true);
            kb_->on_note_on  = [this](int note, float vel) { keyboard_play_on(note, vel); };
            kb_->on_note_off = [this](int note) { keyboard_play_off(note); };
            // Pitch-bend / modulation / sustain pads now reach the voices (RT-safe
            // atomic stores read by the audio thread), matching the host CC path.
            kb_->on_pitch_bend = [this](float b) { pitch_bend_.store(b, std::memory_order_relaxed); };
            kb_->on_modulation = [this](float m) { modulation_.store(m, std::memory_order_relaxed); };
            kb_->on_sustain    = [this](bool on) { sustain_pedal_.store(on, std::memory_order_relaxed); };
            // ⌘K / ⌘W / Escape while the keyboard window is front HIDE it (the
            // toggle hides when visible). Without this the chords only reach the
            // MAIN editor window, so the keyboard could be shown but never hidden
            // once it became key. on_global_key is the keyboard ROOT's hook: the
            // mac host fires it for Escape (keyDown) and Cmd-chords
            // (performKeyEquivalent). performKeyEquivalent runs BEFORE keyDown, so
            // ⌘K / ⌘W are intercepted here and never reach on_key_event as a note.
            kb_->on_global_key = [this](const view::KeyEvent& e) -> bool {
                if (!e.is_down || e.is_repeat) return false;
                const uint16_t cmd = view::kModCmd | view::kModCtrl;
                const bool chord = (e.modifiers & cmd) != 0;
                const bool hide = (chord && e.key == view::KeyCode::k) ||   // ⌘K
                                  (chord && e.key == view::KeyCode::w) ||   // ⌘W
                                  (e.key == view::KeyCode::escape);          // Esc
                if (hide) { toggle_keyboard_window(); return true; }
                return false;
            };
            // 'a' plays slice 0 (note = ROOT). A live listener (create_view) keeps
            // this synced if ROOT changes while the window is open.
            kb_->set_base_note(static_cast<int>(state().get_value(kRootNote)));

            const float w = kb_->panel_width(), h = kb_->panel_height();
            view::WindowOptions opts;
            opts.title = "Musical Typing";
            opts.use_gpu = true;            // faithful SVG needs the Skia Graphite host
            opts.secondary_window = true;   // closing it never quits the standalone app
            opts.width = w;  opts.height = h;
            opts.min_width = w; opts.min_height = 176.0f;  // piano frame is shorter
            opts.resizable = true;
            kb_window_ = view::WindowHost::create(*kb_, opts);
            if (!kb_window_) { kb_.reset(); return; }  // platform without a window host
            // Aspect-locked design viewport (mirror mtk-demo); resize the window to
            // fit the active frame on a piano⇄typing toggle.
            kb_window_->set_design_viewport(w, h);
            kb_window_->set_fixed_aspect_ratio(w / h);
            view::WindowHost* host = kb_window_.get();
            kb_->on_intrinsic_size_changed = [host](float nw, float nh) {
                host->set_fixed_aspect_ratio(nw / nh);
                host->set_design_viewport(nw, nh);
                host->request_content_size(nw, nh);
            };
            // A title-bar close hides the window without going through
            // toggle_keyboard_window's hide branch, so drop the app key monitor here
            // too (otherwise it lingers, harmlessly gated by is_visible(), until the
            // next toggle).
            kb_window_->set_close_callback([this] {
                if (kb_window_) kb_window_->set_app_key_monitor({});
            });
        }
        // Re-sync ROOT each open (it may have changed while the window was hidden).
        if (kb_) {
            kb_->set_base_note(static_cast<int>(state().get_value(kRootNote)));
            kb_->set_input_capture(true);   // re-arm self-capture (dropped on the last hide)
        }
        kb_window_->show();
        // Route app keystrokes to the keyboard while it is open. macOS delivers keys
        // only to the KEY window, so once the user clicks a control in the MAIN editor
        // window (e.g. the tempo fader) the keyboard window stops receiving QWERTY and
        // typing goes dead until it is re-focused. This app-level monitor forwards
        // PLAYABLE keys (notes / 1–8 / tab) to the keyboard regardless of which window
        // is key, consuming ONLY those so everything else still reaches the focused
        // window. Deliberately NOT consumed here (left to the normal window/menu path):
        //   • a focused text input owns the keys (settings chrome, rename field);
        //   • ⌘/Ctrl chords (⌘K/⌘W toggle via the focused window's on_global_key, menu
        //     key-equivalents) and Escape (dismiss a dialog) — consuming these app-wide
        //     would e.g. close the keyboard when Esc was meant for a popover.
        kb_window_->set_app_key_monitor([this](const view::KeyEvent& e) -> bool {
            if (!kb_ || !kb_window_ || !kb_window_->is_visible()) return false;
            if (auto* fv = view::View::focused_input_; fv && fv->accepts_text_input())
                return false;
            const uint16_t chord = view::kModCmd | view::kModCtrl;
            if ((e.modifiers & chord) || e.key == view::KeyCode::escape) return false;
            return kb_->on_key_event(e);   // notes + 1–8 / tab (consumes only what it plays)
        });
    }

    double detected_bpm() const { return loop_bpm_.load(std::memory_order_relaxed); }

    // ── Target-tempo override (UI-driven) ──────────────────────────────────
    // The standalone has no host transport to set a tempo, so a UI control (the
    // footer TEMPO box) drives the target tempo the loop is stretched TO. 0 means
    // "follow the host" (the default — preserves in-DAW host-sync for the headless
    // path / plugin formats that never engage it). Any value in [20,400] pins the
    // render denominator R = loop_bpm / target, so dragging it re-stretches the
    // loop live and the slice regions refit. Off-audio-thread (UI) entry point.
    static constexpr double kTargetBpmMin = 20.0;
    static constexpr double kTargetBpmMax = 400.0;

    // Derive the EXACT loop tempo so the loop tiles the bar grid perfectly at any
    // host tempo. A loop is a whole number of beats, but the analyzer reports an
    // integer (usually fractional-off) bpm, AND it can land an octave off (half /
    // double tempo). Strategy:
    //   1. Infer the analyzer's beat COUNT (round(dur·bpm/60)).
    //   2. Consider that count and its octave neighbours (×¼…×4); each implies an
    //      EXACT tempo N·60/dur that tiles the loop perfectly.
    //   3. Pick the candidate whose tempo is closest to a REFERENCE in octave (log2)
    //      space — the host transport when known, else the analyzer estimate. The
    //      host prior resolves the half/double ambiguity (a loop dropped in a 120
    //      project must be within an octave of 120) and lands the stretch near the
    //      host tempo (R≈1, best quality).
    // The result always makes raw_frames·loop_bpm == N·60·sr, so published_frames =
    // round(N·60·sr / host) is an exact N beats at ANY host tempo. Pure + static so
    // it is unit-testable. `reference_bpm <= 0` ⇒ no host hint (use the analyzer).
    static double bar_exact_bpm(long frames, double sr, double reference_bpm,
                                double analyzer_bpm) {
        if (analyzer_bpm <= 0.0 || frames <= 0 || sr <= 0.0) return analyzer_bpm;
        const double dur = static_cast<double>(frames) / sr;
        if (dur <= 0.0) return analyzer_bpm;
        const double base_beats = std::round(dur * analyzer_bpm / 60.0);
        if (base_beats < 1.0) return analyzer_bpm;                // too short to be a loop
        const double ref = reference_bpm > 0.0 ? reference_bpm : analyzer_bpm;
        const double mults[] = {0.25, 0.5, 1.0, 2.0, 4.0};        // octave neighbours
        double best = 0.0, best_d = 1e30;
        for (double m : mults) {
            const double beats = std::round(base_beats * m);
            if (beats < 1.0) continue;
            const double t = beats * 60.0 / dur;                  // tempo that tiles exactly
            if (t < kTargetBpmMin || t > kTargetBpmMax) continue;
            const double d = std::abs(std::log2(t / ref));        // octave-aware distance
            if (d < best_d) { best_d = d; best = t; }
        }
        return best > 0.0 ? best : analyzer_bpm;
    }
    void set_target_bpm(double bpm) {
        const double clamped = std::clamp(bpm, kTargetBpmMin, kTargetBpmMax);
        tempo_override_.store(clamped, std::memory_order_relaxed);
        request_render(clamped);  // re-stretch to the new target now
    }
    /// The tempo the loop is currently stretched TO. With a manual override it is
    /// that value; otherwise (LINKED) it follows the reference tempo — the loop's
    /// own detected BPM in the standalone (no transport to link to), or the last
    /// host transport tempo in a plugin host.
    double effective_bpm() const {
        const double o = tempo_override_.load(std::memory_order_relaxed);
        if (o > 0.0) return o;
        if (standalone_host_.load(std::memory_order_relaxed)) {
            const double det = loop_bpm_.load(std::memory_order_relaxed);
            if (det > 0.0) return det;
        }
        return pending_host_bpm_.load(std::memory_order_relaxed);
    }
    /// The editor reports whether it is hosted in the standalone chrome (no DAW
    /// transport), so the audio thread can resolve LINKED tempo to the loop's
    /// natural BPM there instead of a host tempo.
    void set_standalone_host(bool on) {
        standalone_host_.store(on, std::memory_order_relaxed);
    }
    /// True once a UI target tempo is engaged (vs. following the host).
    bool tempo_override_active() const {
        return tempo_override_.load(std::memory_order_relaxed) > 0.0;
    }

    /// Tempo LINK state. Linked (override == 0) means the loop follows the host/
    /// transport tempo and re-stretches automatically when the DAW tempo changes;
    /// unlinked means a manual target BPM is engaged. This is the natural reading
    /// of `tempo_override_` (0 = follow host), surfaced for the editor's link
    /// toggle. (Distinct from kTempoLink, which is the pitch+time "vinyl" link.)
    bool tempo_linked() const {
        return tempo_override_.load(std::memory_order_relaxed) <= 0.0;
    }
    /// Toggle tempo LINK. Linking clears the override so playback follows the host;
    /// unlinking seeds the manual target from the currently-engaged (host) tempo so
    /// the slider/number start where the sound is now (no jump). Off-audio-thread.
    void set_tempo_linked(bool linked) {
        if (linked) {
            tempo_override_.store(0.0, std::memory_order_relaxed);  // follow the reference again
            request_render(effective_bpm());  // re-render at the now-linked source tempo
        } else {
            // Unlinking engages the SAMPLE's own tempo — its detected, bar-snapped
            // value — so the loop plays (and the readout shows) its natural
            // fractional tempo, free of the host. Fall back to the current effective
            // tempo only if nothing is detected yet.
            const double det = detected_bpm();
            const double seed = std::clamp(det > 0.0 ? det : effective_bpm(),
                                           kTargetBpmMin, kTargetBpmMax);
            tempo_override_.store(seed, std::memory_order_relaxed);
            request_render(seed);
        }
    }

    std::size_t num_slices() const {
        std::lock_guard<std::mutex> lock(raw_mutex_);
        return slices_orig_.empty() ? 0 : slices_orig_.size() - 1;
    }

    /// Monotonic counter bumped whenever the raw loop or its slices change.
    /// The editor view polls this to know when to refresh (cheap dirty-check).
    std::uint64_t raw_generation() const {
        return raw_generation_.load(std::memory_order_acquire);
    }
    /// Advances ONLY on a new sample load (not on a re-slice). The footer TEMPO
    /// poller watches this so changing the slice count preserves the user's tempo.
    std::uint64_t load_generation() const {
        return load_generation_.load(std::memory_order_acquire);
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

    /// Test-only: the [start,end) frame range region_for_note maps `note` to,
    /// over the currently published (tempo-matched) sample. Returns {0,0} when
    /// the note maps to no slice. Lets tests assert that Play-to-End makes the
    /// root span the whole loop while OFF stops at the next slice boundary.
    std::pair<std::uint64_t, std::uint64_t>
    region_range_for_note_test(int note, bool loop, bool play_to_end) const {
        const auto sample = store_.read_published_view();
        const auto r = region_for_note(note, sample, loop, play_to_end);
        if (!r) return {0, 0};
        return {r->start_frame, r->end_frame};
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
    /// waveform-editor pilot for the Ink & Signal design language.
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

        // Always-visible "Open…" button (top-right). A tap on the WAVEFORM
        // auditions a slice once a sample is loaded, so this is the reliable
        // tap target to load OR replace the sample at any time (drag-drop and
        // the empty-area CTA remain too). Shares the do_browse action wired
        // below — declared as a forward so we can assign it after do_browse.
        TextButton* openBtn = nullptr;
        {
            auto btn = std::make_unique<TextButton>("Open…");
            place(*btn, 656, 14, 84, 28);
            openBtn = btn.get();
            root->add_child(std::move(btn));
        }

        // Toolbar affordances next to "Open…": a "Settings" button that opens the
        // standalone Audio/MIDI device panel, and a "Keyboard" button that toggles
        // the on-screen musical-typing keyboard (same target as ⌘K). Both sit to
        // the left of "Open…" within the clear strip past the title.
        SamplerEditorRoot* root_ptr = root.get();
        {
            // Hidden until update_standalone_chrome_affordances() confirms we are
            // hosted in the standalone settings chrome (no Audio/MIDI tab in a DAW).
            auto btn = std::make_unique<TextButton>("Settings");
            place(*btn, 476, 14, 82, 28);
            btn->set_visible(false);
            btn->on_click = [root_ptr] { format::open_standalone_settings(root_ptr); };
            root->settings_button = btn.get();
            root->add_child(std::move(btn));
        }
        {
            auto btn = std::make_unique<TextButton>("Keyboard");
            place(*btn, 566, 14, 82, 28);
            // The button AND ⌘K hit the SAME path: SamplerEditorRoot::on_toggle_keyboard,
            // which the processor points at toggle_keyboard_window() below.
            btn->on_click = [root_ptr] { root_ptr->toggle_keyboard(); };
            root->add_child(std::move(btn));
        }
        // Both the "Keyboard" button and ⌘K (on_musical_key) route here: open/close
        // the MusicalTypingKeyboard's own secondary GPU window.
        root->on_toggle_keyboard = [this] { toggle_keyboard_window(); };
        // Gate the editor's musical typing on the keyboard window being shown:
        // the editor is the single QWERTY source, so QWERTY only plays slices (and
        // lights the on-screen keys) while the keyboard is on-screen. Hidden
        // keyboard -> typing is silent and nothing lights.
        root->keyboard_window_visible = [this] {
            return kb_window_ && kb_window_->is_visible();
        };

        // ── Waveform / drop area ── (enlarged now the mockup rows are gone).
        // Shows a "drop a sample" CTA when empty; the waveform + slice regions
        // when loaded. Click a slice (or musical-type) to audition it; drop or
        // click-to-browse loads/replaces. Polls the processor to refresh.
        auto wf = std::make_unique<WaveformDropView>();
        place(*wf, 20, 50, 720, 256);  // 20px margins both sides (760 - 2*20)
        wf->on_file_dropped = [this](const std::string& path) { request_load_path(path); };
        // Shared "open a sample" action: used by the empty-area tap CTA AND the
        // always-visible Open button (so you can load/replace even with a sample
        // already loaded, where a waveform tap auditions a slice instead).
        auto do_browse = [this] {
            // Filter from the registry's decodable extensions (incl. m4a/aac/alac/
            // caf on macOS) — strip the leading dot for the dialog's "ext;ext" form.
            std::string pattern;
            for (const auto& e : audio::FormatRegistry::instance().supported_read_extensions()) {
                std::string s = (!e.empty() && e.front() == '.') ? e.substr(1) : e;
                if (s.empty()) continue;
                if (!pattern.empty()) pattern += ';';
                pattern += s;
            }
            auto picked = platform::FileDialog::open_file(
                "Load a sample", {{"Audio Files", pattern}}, "");
            if (picked && !picked->empty()) request_load_path(*picked);
        };
        wf->on_browse = do_browse;
        if (openBtn) openBtn->on_click = do_browse;
        wf->on_play_slice = [this](int idx, bool on) { play_slice(idx, on); };
        wf->playhead_query = [this](int& s, float& p) { playhead(s, p); };
        wf->generation = [this] { return raw_generation(); };
        wf->snapshot = [this](std::vector<float>& mono, float& sr, std::vector<long>& sl) {
            return snapshot_for_view(mono, sr, sl);
        };
        root->add_child(std::move(wf));

        // Musical typing: the SDK MusicalTypingController turns the QWERTY row
        // into notes; the processor remaps those typing notes so the natural
        // home row (a,s,d,f,g,h,j,…) plays consecutive slices. This is the
        // SINGLE computer-keyboard source (the on-screen keyboard is display-only).
        // Notes flow through keyboard_play_on/off, which both plays the slice (same
        // lock-free UI->audio queue as host MIDI / slice clicks) AND lights the
        // matching keys on the on-screen keyboard via set_active_notes. on_musical_key
        // gates this on the keyboard window being visible, so typing is silent and
        // lights nothing when the keyboard is hidden.
        root->current_root_note = [this] { return static_cast<int>(state().get_value(kRootNote)); };
        root->typing.on_note_on = [this](int note, float vel) { keyboard_play_on(note, vel); };
        root->typing.on_note_off = [this](int note) { keyboard_play_off(note); };
        // (The inline editor's `typing` is a note-only MusicalTypingController — it
        // has no pitch-bend / modulation / sustain pads. Those live on the popout
        // MusicalTypingKeyboard widget, wired in toggle_keyboard_window, and on the
        // host CC / pitch-wheel path in process().)
        // Keep the keyboard-window's controller base note synced to ROOT while it
        // is open, so its 'a' key always plays slice 0 (note = root) like the
        // inline editor typing. toggle_keyboard_window() also sets it on open.
        root->listeners.push_back(state().add_listener(
            [this](state::ParamID id, float v) {
                if (id == kRootNote && kb_) kb_->set_base_note(static_cast<int>(v));
            },
            state::ListenerThread::Main));

        // ── Footer: wired controls only ──
        // Root-note dropdown (slice 0 maps to this MIDI note; idx = note - root).
        // Labels follow the requested C-2..C8 convention (C3 = 60); see note_name.
        // Root = octave (C-2..C8). Octave granularity keeps the dropdown short
        // enough to fit + flip above when needed (a 121-note chromatic list was
        // taller than the window and got clipped). idx -> MIDI note idx*12.
        label(20, 324, 34, 18, "ROOT", faint, 10, 600, LabelAlign::left, true);
        {
            auto combo = std::make_unique<ComboBox>();
            std::vector<std::string> names;
            for (int n = kRootNoteMin; n <= kRootNoteMax; n += 12) names.push_back(note_name(n));
            combo->set_items(std::move(names));
            const int cur = static_cast<int>(state().get_value(kRootNote));
            combo->set_selected_silent(std::clamp(cur / 12, 0, kRootNoteMax / 12));
            combo->on_change = [this](int idx) {
                const int note = std::clamp(idx * 12, kRootNoteMin, kRootNoteMax);
                state().begin_gesture(kRootNote);
                state().set_value(kRootNote, static_cast<float>(note));
                state().end_gesture(kRootNote);
            };
            ComboBox* comboPtr = combo.get();
            root->listeners.push_back(state().add_listener(
                [comboPtr](state::ParamID id, float v) {
                    if (id == kRootNote) comboPtr->set_selected_silent(static_cast<int>(v) / 12);
                },
                state::ListenerThread::Main));
            place(*combo, 54, 316, 64, 26);
            root->add_child(std::move(combo));
        }

        // Slice-Mode dropdown (Logic Slice "Mode") + the amount fader beside it.
        // Onset keeps the strongest transients, Beat cuts on the tempo grid, Equal
        // cuts N uniform segments; the fader (kOnsetSens) is the amount control for
        // all three. Either re-slices on the worker (coalesced) via request_reanalyze().
        {
            auto smode = std::make_unique<ComboBox>();
            smode->set_items({"Onset", "Beat", "Equal"});
            smode->set_selected_silent(std::clamp(static_cast<int>(state().get_value(kSliceMode)), 0, 2));
            smode->on_change = [this](int idx) {
                state().begin_gesture(kSliceMode);
                state().set_value(kSliceMode, static_cast<float>(std::clamp(idx, 0, 2)));
                state().end_gesture(kSliceMode);
                request_reanalyze();
            };
            auto* smodePtr = smode.get();
            place(*smode, 124, 316, 64, 26);
            root->listeners.push_back(state().add_listener(
                [this, smodePtr](state::ParamID id, float v) {
                    if (id == kSliceMode) smodePtr->set_selected_silent(std::clamp(static_cast<int>(v), 0, 2));
                },
                state::ListenerThread::Main));
            root->add_child(std::move(smode));
        }
        {
            auto fader = std::make_unique<Fader>();
            fader->set_orientation(Fader::Orientation::horizontal);
            place(*fader, 192, 322, 56, 16);
            root->bindings.push_back(bind_parameter(*fader, state(), kOnsetSens));
            root->add_child(std::move(fader));
            root->listeners.push_back(state().add_listener(
                [this](state::ParamID id, float) {
                    if (id == kOnsetSens) request_reanalyze();
                },
                state::ListenerThread::Main));
        }

        // Target-tempo (TEMPO) box: the standalone has no host transport, so this
        // fader drives the BPM the loop is stretched TO. 20..400 BPM maps to the
        // fader's 0..1; dragging calls set_target_bpm() -> the worker re-renders,
        // so the slice regions refit live. A live numeric readout sits to its
        // right. The FrameTick below defaults both to the detected loop BPM (R≈1)
        // whenever a new loop is analyzed. Mirrors the SENS fader + SLICES readout.
        constexpr double kBpmMin = PulpTempoSamplerProcessor::kTargetBpmMin;  // 20
        constexpr double kBpmMax = PulpTempoSamplerProcessor::kTargetBpmMax;  // 400
        auto bpm_to_norm = [](double b) {
            return static_cast<float>(std::clamp((b - kBpmMin) / (kBpmMax - kBpmMin), 0.0, 1.0));
        };
        auto norm_to_bpm = [](float v) {
            return kBpmMin + static_cast<double>(std::clamp(v, 0.0f, 1.0f)) * (kBpmMax - kBpmMin);
        };
        label(344, 324, 50, 18, "TEMPO", faint, 10, 600, LabelAlign::left, true);
        {
            auto fader = std::make_unique<Fader>();
            fader->set_orientation(Fader::Orientation::horizontal);
            place(*fader, 396, 322, 80, 16);
            fader->set_value(bpm_to_norm(effective_bpm()));
            // Dragging engages a manual target tempo — which auto-UNLINKS from the
            // host (set_target_bpm sets the override). The FrameTick re-syncs the
            // LINK toggle's lit state from tempo_linked() on the next frame.
            fader->on_change = [this, norm_to_bpm](float v) {
                set_target_bpm(norm_to_bpm(v));
            };
            root->tempo_fader = fader.get();
            root->add_child(std::move(fader));
        }
        // Live BPM readout (shows the tempo the loop is currently stretched to —
        // the host tempo while linked, the manual target while unlinked).
        {
            auto live = std::make_unique<LiveText>();
            live->font_family = mono;
            live->color = teal;
            live->text = [this] {
                // One decimal so the loop's bar-snapped fractional tempo is visible
                // (e.g. 103.4 when unlinked) — an integer readout hid it.
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%.1f BPM", effective_bpm());
                return std::string(buf);
            };
            place(*live, 480, 320, 56, 18);
            root->add_child(std::move(live));
        }
        // TEMPO LINK toggle: ON = follow the reference tempo (host transport in a
        // DAW; the loop's detected BPM in standalone) and re-stretch automatically
        // when it changes. OFF = the manual TEMPO fader value. Tapping it re-links
        // (clears the override) or unlinks (seeds the override from the current
        // tempo, no jump). Defaults ON, so a fresh plugin instance follows the DAW.
        {
            auto linkBtn = std::make_unique<ToggleButton>();
            linkBtn->set_label("LINK");
            linkBtn->set_font_size(10.0f);
            linkBtn->set_corner_radius(6.0f);
            linkBtn->set_on_background_color(teal);
            linkBtn->set_on_text_color(bg900);
            linkBtn->set_off_background_color(raised);
            linkBtn->set_off_text_color(muted);
            linkBtn->set_off_border_color(faint);
            linkBtn->set_on(tempo_linked());
            linkBtn->on_toggle = [this](bool on) { set_tempo_linked(on); };
            place(*linkBtn, 542, 316, 64, 26);
            root->link_btn = linkBtn.get();
            root->add_child(std::move(linkBtn));
        }

        // Live slice count, in green and sitting RIGHT OF THE SENS fader (which
        // drives it) — mirroring how the BPM readout sits right of the TEMPO fader.
        {
            auto live = std::make_unique<LiveText>();
            live->font_family = mono;
            live->color = teal;
            live->text = [this] { return "SLICES  " + std::to_string(num_slices()); };
            place(*live, 252, 320, 84, 18);
            root->add_child(std::move(live));
        }

        // Per-frame poller: keep the TEMPO fader knob + LINK lit-state in sync with
        // the live effective tempo. While LINKED the fader visually tracks the
        // reference tempo (host transport / detected BPM) as it changes; while
        // unlinked it matches the manual value (a no-op). Fader::set_value is silent
        // (no on_change), so this never spuriously unlinks. A fresh loop stays in
        // its current LINK state — no forced override — so a plugin instance follows
        // the DAW by default and the standalone plays the loop at its natural tempo.
        {
            auto tick = std::make_unique<FrameTick>();
            SamplerEditorRoot* rp = root.get();
            tick->on_tick = [this, rp, bpm_to_norm] {
                rp->tempo_seen_gen_ = load_generation();  // tracked for parity; no reset
                if (rp->tempo_fader)
                    rp->tempo_fader->set_value(bpm_to_norm(effective_bpm()));
                if (rp->link_btn && rp->link_btn->is_on() != tempo_linked())
                    rp->link_btn->set_on(tempo_linked());
            };
            place(*tick, 0, 0, 0, 0);
            root->add_child(std::move(tick));
        }

        // MODE dropdown: the loop behavior (Logic Quick Sampler-style). "1-Shot"
        // disables looping (kTempoLoop=0); the looping modes set kTempoLoop=1 and
        // pick the direction via kLoopMode (0 Forward, 1 Reverse). One control
        // drives both params; region_for_note resolves playback_mode per note.
        {
            auto combo = std::make_unique<ComboBox>();
            combo->set_items({"1-Shot", "Forward", "Reverse", "Ping-Pong"});
            auto sel_for_state = [this]() -> int {
                if (state().get_value(kTempoLoop) < 0.5f) return 0;  // one-shot
                return std::clamp(static_cast<int>(state().get_value(kLoopMode)) + 1, 1, 3);
            };
            combo->set_selected_silent(sel_for_state());
            combo->on_change = [this](int idx) {
                if (idx <= 0) {
                    state().begin_gesture(kTempoLoop);
                    state().set_value(kTempoLoop, 0.0f);
                    state().end_gesture(kTempoLoop);
                } else {
                    state().begin_gesture(kLoopMode);
                    state().set_value(kLoopMode, static_cast<float>(idx - 1));
                    state().end_gesture(kLoopMode);
                    state().begin_gesture(kTempoLoop);
                    state().set_value(kTempoLoop, 1.0f);
                    state().end_gesture(kTempoLoop);
                }
            };
            auto* modePtr = combo.get();
            place(*combo, 614, 316, 64, 26);
            root->listeners.push_back(state().add_listener(
                [this, modePtr, sel_for_state](state::ParamID id, float) {
                    if (id == kTempoLoop || id == kLoopMode)
                        modePtr->set_selected_silent(sel_for_state());
                },
                state::ListenerThread::Main));
            root->add_child(std::move(combo));
        }

        // TO END toggle (Logic Quick Sampler "Play to End"): when on, a triggered
        // slice plays through to the loop end, so the root key yields the whole
        // loop and raising SLICES never steals whole-loop looping. Off = strict
        // per-slice chop. Bound to kPlayToEnd (default ON); region_for_note reads
        // it per note. Held voices pick up a TO END change on the next trigger.
        {
            auto endBtn = std::make_unique<ToggleButton>();
            endBtn->set_label("TO END");
            endBtn->set_font_size(10.0f);
            endBtn->set_corner_radius(6.0f);
            endBtn->set_on_background_color(teal);
            endBtn->set_on_text_color(bg900);
            endBtn->set_off_background_color(raised);
            endBtn->set_off_text_color(muted);
            endBtn->set_off_border_color(faint);
            place(*endBtn, 682, 316, 58, 26);
            root->bindings.push_back(bind_parameter(*endBtn, state(), kPlayToEnd));
            root->add_child(std::move(endBtn));
        }

        // The musical-typing keyboard is NOT inline anymore: the SDK primitive
        // pulp::view::MusicalTypingKeyboard is hosted in its OWN secondary GPU
        // window (⌘K / the "Keyboard" button → toggle_keyboard_window()), so it
        // never overlaps the waveform editor.

        // ── Main / Detail page toggle (header) ──
        // A single button that flips between the main slice page and the Detail
        // page; its label tracks the page you'd switch TO (Logic-style two-page UI).
        {
            auto btn = std::make_unique<TextButton>("Detail");
            SamplerEditorRoot* rp = root.get();
            auto* btnPtr = btn.get();
            btn->on_click = [rp, btnPtr, shown = std::make_shared<bool>(false)] {
                *shown = !*shown;
                if (rp->detail_overlay_) rp->detail_overlay_->set_visible(*shown);
                btnPtr->set_label(*shown ? "Main" : "Detail");
                rp->request_repaint();
            };
            place(*btn, 384, 14, 82, 28);
            root->add_child(std::move(btn));
        }

        // ── Detail page (Logic "Q-Sampler Detail"-style) ──
        // An opaque overlay over the body (y>=50, header stays visible) holding the
        // envelope-adjacent controls. Hidden until the Detail page toggle selects it.
        // Gate + Flex Speed live here now; trim / fades / envelopes land here next.
        {
            auto detail = std::make_unique<View>();
            place(*detail, 0, 50, 760, 322);
            detail->set_background_color(bg900);   // opaque: hides the body behind it
            detail->set_visible(false);
            auto dlabel = [&](float x, float y, float w, const char* t, Color col, float sz, int wt) {
                auto l = std::make_unique<Label>(std::string(t));
                place(*l, x, y, w, 18);
                l->set_text_color(col); l->set_font_size(sz); l->set_font_weight(wt);
                l->set_text_align(LabelAlign::left); detail->add_child(std::move(l));
            };
            dlabel(20, 16, 200, "DETAIL", textStr, 13, 600);

            // GATE: key-up releases the envelope (on) vs One-Shot play-through (off).
            dlabel(40, 70, 70, "GATE", faint, 10, 600);
            {
                auto g = std::make_unique<ToggleButton>();
                g->set_label("GATE"); g->set_font_size(10.0f); g->set_corner_radius(6.0f);
                g->set_on_background_color(teal); g->set_on_text_color(bg900);
                g->set_off_background_color(raised); g->set_off_text_color(muted);
                g->set_off_border_color(faint);
                place(*g, 130, 64, 70, 26);
                root->bindings.push_back(bind_parameter(*g, state(), kGate));
                detail->add_child(std::move(g));
            }

            // FLEX SPEED: 1/4..4x of the tempo-synced playback speed (Logic Flex Speed).
            dlabel(40, 112, 90, "FLEX SPEED", faint, 10, 600);
            {
                auto fs = std::make_unique<ComboBox>();
                fs->set_items({"1/4x", "1/2x", "1x", "2x", "4x"});
                fs->set_selected_silent(std::clamp(static_cast<int>(state().get_value(kFlexSpeed)), 0, 4));
                fs->on_change = [this](int idx) {
                    state().begin_gesture(kFlexSpeed);
                    state().set_value(kFlexSpeed, static_cast<float>(std::clamp(idx, 0, 4)));
                    state().end_gesture(kFlexSpeed);
                    request_render(effective_bpm());   // re-stretch at the new speed
                };
                auto* fsPtr = fs.get();
                place(*fs, 130, 106, 72, 26);
                root->listeners.push_back(state().add_listener(
                    [this, fsPtr](state::ParamID id, float v) {
                        if (id == kFlexSpeed) fsPtr->set_selected_silent(std::clamp(static_cast<int>(v), 0, 4));
                    },
                    state::ListenerThread::Main));
                detail->add_child(std::move(fs));
            }

            dlabel(40, 168, 520, "Trim, fades & envelopes — coming next", muted, 10, 400);

            root->detail_overlay_ = detail.get();
            root->add_child(std::move(detail));   // LAST = topmost over the body
        }

        root->layout_children();
        return root;
    }

    // Once the editor is attached to the window (and, in the standalone, parented
    // into the settings chrome), resolve chrome-dependent affordances — i.e.
    // reveal the "Settings" button only when the Audio/MIDI panel is actually
    // reachable. In a DAW there is no such chrome, so the button stays hidden.
    void on_view_opened(view::View& view) override {
        if (auto* root = dynamic_cast<SamplerEditorRoot*>(&view)) {
            root->update_standalone_chrome_affordances();
            // Tell the audio thread whether to resolve LINKED tempo to the loop's
            // detected BPM (standalone) or the host transport (DAW).
            set_standalone_host(root->hosted_in_standalone_);
        }
    }

    // ── Audio thread ───────────────────────────────────────────────────────

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        clear_output(output);

        // Track the effective tempo; on change, ask the worker to re-render
        // (RT-safe: just atomic loads/stores + a flag, no allocation/locks on the
        // audio thread). When the UI engages a target-tempo OVERRIDE (the footer
        // TEMPO box / set_target_bpm), it wins over the host tempo — and we render
        // to it, NOT the host value, so this per-block read can never clobber the
        // user's pending_host_bpm_ back to the host's tempo.
        const double host_bpm = ctx.tempo_bpm > 0.0 ? ctx.tempo_bpm : 120.0;
        const double override_bpm = tempo_override_.load(std::memory_order_relaxed);
        // Tempo source: a manual override wins; otherwise LINKED — follow the loop's
        // own detected BPM in the standalone (no transport to link to), else the host
        // transport. This makes a fresh plugin instance follow the DAW by default,
        // while the standalone keeps playing loops at their natural tempo.
        double bpm;
        if (override_bpm > 0.0) {
            bpm = override_bpm;
        } else if (standalone_host_.load(std::memory_order_relaxed)) {
            const double det = loop_bpm_.load(std::memory_order_relaxed);
            bpm = det > 0.0 ? det : host_bpm;
        } else {
            bpm = host_bpm;
        }
        if (ctx.tempo_changed || bpm != last_host_bpm_) {
            last_host_bpm_ = bpm;
            request_render(bpm);
        }

        const auto published = store_.read_published_view();
        const bool can_trigger = store_.slot_view_valid(published);
        const auto params = current_params();
        const auto block_frames = static_cast<std::uint32_t>(output.num_samples());
        midi_in.sort();

        // Sustain pedal: when the damper lifts, release every voice that a
        // note-off left held (pedal-down deferred the release in release_note).
        const bool pedal = sustain_pedal_.load(std::memory_order_relaxed);
        if (sustain_prev_ && !pedal)
            for (auto& voice : voices_)
                if (voice.active && voice.sustained) { voice.release(); voice.sustained = false; }
        sustain_prev_ = pedal;

        // LOOP toggle acts on currently-HELD notes (the natural expectation): turning
        // it ON engages looping for every held note — re-triggering even a finished
        // one-shot so you don't have to re-press the key; turning it OFF switches
        // active voices to OneShot so a held loop stops at the end of its current pass
        // (no restart, no click). The held set is keyed by note-on/off, so a key still
        // physically down is "held" even if its one-shot already played through.
        if (params.loop != loop_prev_) {
            if (params.loop) {
                for (int n = 0; n < 128; ++n) {
                    if (held_velocity_[static_cast<size_t>(n)] <= 0.0f) continue;
                    bool has = false;
                    for (auto& v : voices_)
                        if (v.active && v.note == n && !v.released) {
                            v.region.playback_mode = params.loop_mode;
                            v.renderer.set_playback_mode(params.loop_mode);
                            has = true;
                        }
                    // Re-trigger a finished one-shot ONLY into a free voice. Stealing
                    // voices_[0] here would clobber a voice we just switched to looping
                    // earlier in this same scan (and bounds the per-block trigger
                    // fan-out). A held note with no spare voice simply isn't re-looped —
                    // no worse than before LOOP engaged.
                    bool free_voice = false;
                    for (auto& v : voices_) if (!v.active) { free_voice = true; break; }
                    if (!has && free_voice && can_trigger)
                        trigger_note(n, held_velocity_[static_cast<size_t>(n)], published, params);
                }
            } else {
                for (auto& v : voices_)
                    if (v.active) {
                        v.region.playback_mode = audio::LoopPlaybackMode::OneShot;
                        v.renderer.set_playback_mode(audio::LoopPlaybackMode::OneShot);
                    }
            }
            loop_prev_ = params.loop;
        }

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
            else if (event.message.isController()) {
                // Mod wheel (CC1) -> vibrato depth; sustain (CC64) -> damper hold.
                const int cc = event.message.getControllerNumber();
                const float v = static_cast<float>(event.message.getControllerValue()) / 127.0f;
                if (cc == 1) modulation_.store(v, std::memory_order_relaxed);
                else if (cc == 64) sustain_pedal_.store(v >= 0.5f, std::memory_order_relaxed);
                else if (cc == 120 || cc == 123) {
                    // Panic: All Sound Off (120) hard-stops every voice; All Notes Off
                    // (123) releases them. Either way the held set MUST clear, or a
                    // later LOOP-on would resurrect a phantom chord the host already
                    // silenced.
                    for (auto& voice : voices_)
                        if (voice.active) { if (cc == 120) voice.reset(); else voice.release(); }
                    std::fill(std::begin(held_velocity_), std::end(held_velocity_), 0.0f);
                    if (cc == 120) sustain_pedal_.store(false, std::memory_order_relaxed);
                }
            } else if (event.message.isPitchWheel()) {
                // 14-bit pitch wheel: LSB | (MSB<<7), center 8192 -> -1..+1.
                const std::uint8_t* d = event.message.data();
                const int val = static_cast<int>(d[1]) | (static_cast<int>(d[2]) << 7);
                pitch_bend_.store((static_cast<float>(val) - 8192.0f) / 8192.0f,
                                  std::memory_order_relaxed);
            }
            cursor = offset;
        }
        if (cursor < block_frames) render_active_voices(output, cursor, block_frames - cursor, params);

        // Master soft-limit on the summed voice mix. Voices add into the output with
        // no per-voice headroom, so overlapping slices can sum past full-scale and
        // clip the device — most audible at slower tempos, where each slice is
        // stretched LONGER and successive triggers overlap more. The limiter is
        // transparent below the knee (the un-stretched / lightly-played sound is
        // unchanged) and smoothly bounds peaks to +/-1.0 above it, replacing hard
        // digital clipping with a gentle ceiling.
        const auto mix_ch = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(output.num_channels()), prepared_output_channels_);
        for (std::uint32_t ch = 0; ch < mix_ch; ++ch) {
            float* d = output.channel_ptr(ch);
            for (std::uint32_t i = 0; i < block_frames; ++i) d[i] = soft_limit(d[i]);
        }

        publish_playhead();
        publish_audio_acknowledgement(published);
    }

    // Transparent below +/-kLimitKnee, smoothly compressing to a hard +/-1.0 ceiling
    // above it (tanh shoulder, C1-continuous at the knee). Cheap: the tanh only runs
    // on the rare samples that exceed the knee.
    static float soft_limit(float x) noexcept {
        constexpr float kLimitKnee = 0.9f;
        const float a = std::fabs(x);
        if (a <= kLimitKnee) return x;
        const float s = x < 0.0f ? -1.0f : 1.0f;
        return s * (kLimitKnee + (1.0f - kLimitKnee) *
                                     std::tanh((a - kLimitKnee) / (1.0f - kLimitKnee)));
    }

private:
    struct RenderParams {
        float gain = 1.0f;
        signal::Adsr::Params adsr;
        bool loop = false;
        bool play_to_end = true;  // triggered slice plays to the loop end (root => whole loop)
        audio::LoopPlaybackMode loop_mode = audio::LoopPlaybackMode::Forward;  // direction when looping
    };

    // Publish the most-recently-triggered active voice's slice + progress for
    // the UI playhead (audio thread; lock-free atomics).
    void publish_playhead() noexcept {
        const int root = static_cast<int>(state().get_value(kRootNote));
        for (const auto& v : voices_) {
            if (v.active && v.note == last_note_) {
                const double start = static_cast<double>(v.region.start_frame);
                const double len = static_cast<double>(v.region.end_frame) - start;
                float pos = 0.0f;
                if (len > 0.0)
                    pos = static_cast<float>(
                        std::clamp((v.renderer.position() - start) / len, 0.0, 1.0));
                ui_play_slice_.store(v.note - root, std::memory_order_release);
                ui_play_pos_.store(pos, std::memory_order_relaxed);
                return;
            }
        }
        ui_play_slice_.store(-1, std::memory_order_release);  // nothing playing
    }

    // Resample the just-loaded raw audio from its native rate to the host
    // sample rate so analysis, stretching, and playback all share one
    // coordinate system. Without this, a sub-host-rate file (e.g. a 44.1k
    // sample loaded into a 48k host) plays back ~host/native faster — audibly
    // higher pitched — because both the OfflineStretch engine and the
    // PublishedSampleStore are pinned to host_sample_rate_ while the raw data
    // sits at its file rate. Runs under raw_mutex_, off the audio thread.
    // Linear-phase: the leading group delay is trimmed so the downbeat stays
    // put, and the output is length-locked to round(N * out/in).
    void resample_raw_to_host_locked() {
        const double in_sr = raw_sr_;
        const double out_sr = static_cast<double>(host_sample_rate_);
        if (in_sr <= 0.0 || out_sr <= 0.0) return;
        if (std::abs(in_sr - out_sr) < 1.0) return;        // already at host rate
        if (raw_frames_ <= 0 || raw_channels_ < 1) return;

        const std::size_t in_n = static_cast<std::size_t>(raw_frames_);
        const std::size_t ch = static_cast<std::size_t>(std::min(raw_channels_, 2));
        signal::Resampler rs;
        rs.prepare(in_sr, out_sr, ch, in_n);

        const std::size_t tpp = rs.taps_per_phase();
        const std::size_t gd_in = (tpp > 0) ? (tpp - 1) / 2 : 0;   // input-rate group delay
        const std::size_t gd_out = static_cast<std::size_t>(
            std::llround(static_cast<double>(gd_in) * out_sr / in_sr));
        const long want = std::lround(static_cast<double>(raw_frames_) * out_sr / in_sr);
        if (want <= 0) return;

        const std::size_t cap = rs.max_output_for(in_n + gd_in) + 8u;
        std::array<std::vector<float>, 2> out;
        std::array<float*, 2> outp{};
        for (int c = 0; c < 2; ++c) { out[c].assign(cap, 0.0f); outp[c] = out[c].data(); }

        // Pass 1: real input.
        std::array<const float*, 2> inp{raw_[0].data(), raw_[1].data()};
        const auto r1 = rs.process_block_detailed(inp.data(), in_n, outp.data(), cap);
        std::size_t produced = r1.output_frames;

        // Pass 2: flush zeros to drain the filter's delayed tail.
        std::array<std::vector<float>, 2> zeros;
        std::array<const float*, 2> zp{};
        for (int c = 0; c < 2; ++c) { zeros[c].assign(gd_in, 0.0f); zp[c] = zeros[c].data(); }
        std::array<float*, 2> outp2{out[0].data() + produced, out[1].data() + produced};
        if (gd_in > 0 && produced < cap) {
            const auto r2 = rs.process_block_detailed(zp.data(), gd_in, outp2.data(), cap - produced);
            produced += r2.output_frames;
        }

        // Drop the linear-phase latency, then length-lock to `want` frames
        // (zero-pad if the flush came up short).
        const std::size_t start = std::min<std::size_t>(gd_out, produced);
        const std::size_t avail = produced - start;
        for (int c = 0; c < 2; ++c) {
            std::vector<float> trimmed(static_cast<std::size_t>(want), 0.0f);
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(want), avail);
            std::copy(out[c].begin() + start, out[c].begin() + start + n, trimmed.begin());
            raw_[c].swap(trimmed);
        }
        raw_frames_ = want;
        raw_sr_ = out_sr;
    }

    // ── Analysis (under raw_mutex_) ──
    // A clean loop is an integer number of BEATS, but the analyzer reports an
    // INTEGER bpm — and a loop's true tempo is usually fractional, so stretching to
    // the rounded value makes the loop tile slightly short/long ("ends a bit early",
    // and the error scales when slowed). Use the rough bpm only to infer the beat
    // COUNT, then derive the exact tempo that makes the file an exact musical length,
    // so it loops perfectly at ANY host tempo. Only snaps when the file is already
    // close to a whole-beat length (a real loop); otherwise keeps the raw estimate
    // so non-loop one-shots aren't distorted. raw_mutex_ is held by analyze_locked.
    double snap_loop_bpm(double analyzer_bpm) const {
        // The last host transport tempo (project tempo in a DAW) is the prior that
        // resolves the half/double-tempo ambiguity; 0 in a fresh standalone falls
        // back to the analyzer estimate inside bar_exact_bpm.
        return bar_exact_bpm(raw_frames_, raw_sr_,
                             pending_host_bpm_.load(std::memory_order_relaxed), analyzer_bpm);
    }

    void analyze_locked() {
        std::array<const float*, 2> ptrs{raw_[0].data(), raw_[1].data()};
        audio::BufferView<const float> view(ptrs.data(), static_cast<std::size_t>(raw_channels_),
                                            static_cast<std::size_t>(raw_frames_));
        audio::BuiltInKeyTempoAnalyzer kt;
        audio::KeyTempoAnalysisConfig kc;
        kc.source_sample_rate = raw_sr_;
        kc.channels = static_cast<std::uint32_t>(raw_channels_);
        const auto kr = kt.analyze(view, kc);
        loop_bpm_.store(snap_loop_bpm(kr.tempo_bpm), std::memory_order_relaxed);

        // Collect a generous candidate set, then keep the strongest cuts by
        // confidence so sensitivity maps DIRECTLY to a slice COUNT (predictable
        // UX): s=0 -> 1 slice (whole sample), s=1 -> up to kMaxSlices.
        audio::OnsetDetector od;
        audio::OnsetDetectionConfig oc;
        oc.threshold_multiplier = 1.0;   // generous — gather candidates
        oc.max_markers = 256;
        const auto onsets = od.detect(view, oc);

        const double s = std::clamp(static_cast<double>(state().get_value(kOnsetSens)), 0.0, 1.0);
        const int target = 1 + static_cast<int>(std::lround(s * (kMaxSlices - 1)));  // 1..kMaxSlices
        const int keep = std::max(0, target - 1);  // number of cut points

        // Minimum slice length (~40 ms) so dense onsets never produce a sliver clip
        // — worst at high sensitivity, and at the FIRST slice [0, first cut) which
        // OnsetDetector's inter-onset spacing doesn't guard. Snap radius (~5 ms) so
        // each cut lands on a zero-crossing: a slice boundary is shared by the end
        // of one slice and the start of the next, so snapping it declicks BOTH edges.
        const long kMinSlice = std::max<long>(256, std::lround(0.040 * raw_sr_));
        const long kSnap = std::max<long>(32, std::lround(0.005 * raw_sr_));
        auto sum_at = [&](long i) {
            float v = 0.0f;
            for (int c = 0; c < raw_channels_; ++c) v += raw_[c][static_cast<size_t>(i)];
            return v;
        };
        auto snap_zc = [&](long f) {
            const long lo = std::max(1L, f - kSnap), hi = std::min(raw_frames_ - 1, f + kSnap);
            long best = f, bestd = kSnap + 1;
            for (long i = lo; i <= hi; ++i)
                if ((sum_at(i - 1) <= 0.0f) != (sum_at(i) <= 0.0f)) {
                    const long d = std::llabs(i - f);
                    if (d < bestd) { bestd = d; best = i; }
                }
            return best;
        };

        // Slicing mode (Logic Slice "Mode" pop-up). Onset (default) keeps the
        // strongest transients; Equal cuts N uniform segments; Beat cuts on the
        // detected tempo's (sub)beat grid. SENS is the amount control for all.
        const int slice_mode = static_cast<int>(state().get_value(kSliceMode) + 0.5f);
        std::vector<long> cuts;
        if (slice_mode == 2) {
            // Equal divisions: s -> N equal segments (1..kMaxSlices).
            const int n = 1 + static_cast<int>(std::lround(s * (kMaxSlices - 1)));
            for (int k = 1; k < n; ++k) {
                const long f = snap_zc(static_cast<long>(
                    std::llround(static_cast<double>(raw_frames_) * k / n)));
                if (f >= kMinSlice && raw_frames_ - f >= kMinSlice &&
                    (cuts.empty() || f - cuts.back() >= kMinSlice)) cuts.push_back(f);
            }
        } else if (slice_mode == 1) {
            // Beat divisions: cut every (sub)beat of the detected loop tempo.
            // s -> {1,2,4,8,16} subdivisions per beat.
            const double bpm = loop_bpm_.load(std::memory_order_relaxed);
            static constexpr int kDivs[] = {1, 2, 4, 8, 16};
            const int di = std::clamp(static_cast<int>(std::lround(s * 4.0)), 0, 4);
            const double step = bpm > 0.0
                ? (60.0 / bpm * raw_sr_) / static_cast<double>(kDivs[di]) : 0.0;
            if (step >= static_cast<double>(kMinSlice))
                for (double f = step;
                     f < static_cast<double>(raw_frames_ - 1) &&
                     static_cast<int>(cuts.size()) < kMaxSlices - 1; f += step) {
                    const long c = snap_zc(static_cast<long>(std::llround(f)));
                    if (c >= kMinSlice && raw_frames_ - c >= kMinSlice &&
                        (cuts.empty() || c - cuts.back() >= kMinSlice)) cuts.push_back(c);
                }
        } else {
            // Onset/Transient (default): greedily accept the highest-confidence
            // cuts (preserving the sensitivity->count mapping) that keep EVERY
            // slice — incl. first and last — at least kMinSlice long, each snapped
            // to a zero-crossing so both shared edges declick.
            std::vector<audio::OnsetMarker> cand;
            for (const auto& m : onsets.markers)
                if (static_cast<long>(m.frame) > 0 && static_cast<long>(m.frame) < raw_frames_)
                    cand.push_back(m);
            std::sort(cand.begin(), cand.end(),
                      [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
            for (const auto& m : cand) {
                if (static_cast<int>(cuts.size()) >= keep) break;
                const long f = snap_zc(static_cast<long>(m.frame));
                if (f < kMinSlice || raw_frames_ - f < kMinSlice) continue;
                bool ok = true;
                for (long c : cuts)
                    if (std::llabs(c - f) < kMinSlice) { ok = false; break; }
                if (ok) cuts.push_back(f);
            }
        }
        std::sort(cuts.begin(), cuts.end());

        slices_orig_.clear();
        slices_orig_.push_back(0);
        for (long c : cuts) slices_orig_.push_back(c);
        slices_orig_.push_back(raw_frames_);
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
        // Flex Speed: playing N x faster shortens the stretched duration by N.
        R /= flex_speed_factor();
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
        // Graft verbatim transients back onto the stretched output so drum/loop
        // attacks keep their punch instead of the phase-vocoder "compressed" smear.
        // No-op on tonal material (no onsets detected) and on the pitch/linked
        // paths; active on the tempo-only spectral render the sampler uses most.
        o.transient_mode = signal::StretchTransientMode::verbatim_relocate;

        const long out_frames = signal::offline_stretch_output_frames(frames, R);
        if (out_frames <= 0) return;
        // The published store has a fixed per-slot capacity; a stretched length
        // beyond it would be REJECTED by load_*, skipping the publish and leaving
        // every slice silent (the longer-sample bug). Clamp the published length
        // so an oversize sample plays its head instead of nothing — slices past
        // pub_frames map past the buffer and region_for_note already silences them.
        const long pub_frames =
            std::min<long>(out_frames, static_cast<long>(SamplerSampleStore::kMaxFrames));

        // NOTE: slices_stretched_ is refreshed AFTER a successful publish below, not
        // here. The slice boundaries are scaled by the time-ratio R, so they only
        // match the buffer that was rendered at that same R. If the re-publish is
        // SKIPPED (a held voice still occupies a store slot, ok == false), updating
        // the slices early would point the trigger mapping at the new R while the
        // live buffer is still the old R — every note maps past the buffer and goes
        // silent until a later render lands (the "adjust tempo -> sampler stops
        // triggering, adjust again -> fixed" bug). Keeping the old slices until the
        // matching audio is actually published keeps the sampler playable through a
        // skipped publish; the new tempo/slicing applies once the slot frees.

        std::vector<std::vector<float>> stretched(static_cast<size_t>(ch),
                                                  std::vector<float>(static_cast<size_t>(out_frames)));
        std::vector<const float*> inp(static_cast<size_t>(ch));
        std::vector<float*> outp(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) { inp[c] = raw_copy[static_cast<size_t>(c)].data(); outp[c] = stretched[static_cast<size_t>(c)].data(); }

        // Material-adaptive FFT window — analyze the loop and size the STFT to its
        // character instead of the fixed 4096/512 default: a percussive break gets
        // 1024/128 (sharp, bright attacks — the "drum_pl" reference render), a
        // bass-heavy loop gets 8192/512 (resolves low partials, no wobble). This is
        // the adaptive-FFT the engine implements but the sampler had never wired, so
        // every loop was rendered at one window — smeary on drums. Pass the window
        // (and the engine's prepared range) to the worker-local engine's prepare().
        const auto win = signal::OfflineStretch::recommend_window(
            inp.data(), frames, ch, static_cast<double>(host_sample_rate_));
        o.fft_size = win.fft_size;
        o.analysis_hop = win.analysis_hop;
        o.max_time_ratio = engine_.max_time_ratio();
        o.max_pitch_semitones = engine_.max_pitch_semitones();

        signal::OfflineStretch local;          // worker-local engine instance
        local.prepare(static_cast<double>(host_sample_rate_), ch, o);
        std::string err;
        if (!local.process(inp.data(), frames, outp.data(), out_frames, o, &err)) return;

        // Publish the stretched buffer (generation-safe; in-flight voices keep
        // their old generation until they finish — RT lifetime via the store).
        const auto gen = audio_ack_generation_.load(std::memory_order_acquire);
        bool ok = false;
        if (ch == 1) {
            ok = store_.load_mono(stretched[0].data(), static_cast<int>(pub_frames),
                                  host_sample_rate_, gen);
        } else {
            std::vector<float> inter(static_cast<size_t>(pub_frames) * 2);
            for (long i = 0; i < pub_frames; ++i) {
                inter[static_cast<size_t>(i) * 2] = stretched[0][static_cast<size_t>(i)];
                inter[static_cast<size_t>(i) * 2 + 1] = stretched[1][static_cast<size_t>(i)];
            }
            ok = store_.load_interleaved_stereo(inter.data(), static_cast<int>(pub_frames),
                                                host_sample_rate_, gen);
        }
        if (!ok) return;

        // Publish succeeded — NOW refresh the slice boundaries so they always match
        // the buffer that was just published at this R. (On a skipped publish we
        // returned above with the old, still-consistent slices intact.)
        {
            std::vector<long> scaled;
            scaled.reserve(slices.size());
            for (long s : slices)
                scaled.push_back(std::min<long>(pub_frames, static_cast<long>(std::llround(s * R))));
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
        p.play_to_end = state().get_value(kPlayToEnd) >= 0.5f;
        switch (static_cast<int>(state().get_value(kLoopMode))) {
            case 1:  p.loop_mode = audio::LoopPlaybackMode::Reverse; break;
            case 2:  p.loop_mode = audio::LoopPlaybackMode::PingPong; break;
            default: p.loop_mode = audio::LoopPlaybackMode::Forward; break;
        }
        return p;
    }

    static void clear_output(audio::BufferView<float>& output) noexcept {
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
            std::fill_n(output.channel_ptr(ch), output.num_samples(), 0.0f);
    }

    // Region for a note: a slice of the published (already tempo-matched) buffer.
    std::optional<audio::LoopRegion> region_for_note(
        int note, const audio::PublishedSampleView& sample, bool loop, bool play_to_end,
        audio::LoopPlaybackMode loop_mode = audio::LoopPlaybackMode::Forward) const noexcept {
        std::uint64_t start = 0, end = 0;
        const int root = static_cast<int>(state().get_value(kRootNote));
        {
            std::lock_guard<std::mutex> lock(slice_mutex_);
            if (slices_stretched_.size() >= 2) {
                const int idx = note - root;
                if (idx >= 0 && idx + 1 < static_cast<int>(slices_stretched_.size())) {
                    start = static_cast<std::uint64_t>(slices_stretched_[static_cast<size_t>(idx)]);
                    // Play to End (Logic Quick Sampler): the triggered slice plays
                    // through to the loop end — the last stretched marker, which
                    // analyze_locked()/render_to_tempo() guarantee equals the
                    // published loop end — instead of stopping at the next slice.
                    // So idx 0 (root) => the whole loop; idx k => slice k to end.
                    // OFF => stop at the next boundary (strict per-slice chop).
                    end = play_to_end
                              ? static_cast<std::uint64_t>(slices_stretched_.back())
                              : static_cast<std::uint64_t>(slices_stretched_[static_cast<size_t>(idx) + 1]);
                }
            }
        }
        // Defensive: the published loop end can equal num_frames; clamp so the
        // whole-loop region is never rejected by the > num_frames guard below.
        if (play_to_end && end > sample.num_frames) end = sample.num_frames;
        // Each slice is mapped to its own chromatic note (idx = note - root). A
        // note outside [root, root + num_slices) — or one with invalid stretched
        // boundaries — maps to NO slice: play nothing. Never fall back to the
        // whole sample (that mapped the entire sample across the keyboard, the
        // Reaper/host MIDI bug).
        if (end <= start || end > sample.num_frames) return std::nullopt;
        audio::LoopRegion region;
        region.start_frame = start;
        region.end_frame = end;
        region.source_sample_rate = sample.sample_rate;
        region.playback_mode = loop ? loop_mode : audio::LoopPlaybackMode::OneShot;
        region.interpolation = audio::LoopInterpolationMode::Linear;
        region.crossfade_curve = audio::LoopCrossfadeCurve::Linear;
        return region;
    }

    // Playback-rate multiplier for one render chunk: pitch-bend (a static
    // semitone offset over +/- kBendRangeSemis) plus mod-wheel vibrato (a
    // kVibratoHz LFO whose phase advances by `chunk` frames, depth scaled by the
    // mod amount). Audio-thread only; returns exactly 1.0 when nothing is
    // engaged so the common no-expression path is allocation- and transcendental-
    // free.
    float apply_expression_rate(std::uint32_t chunk) noexcept {
        constexpr double kTwoPi = 6.283185307179586;
        const float bend = pitch_bend_.load(std::memory_order_relaxed);
        const float mod = modulation_.load(std::memory_order_relaxed);
        const double sr = host_sample_rate_ > 0.0f ? host_sample_rate_ : 48000.0;
        mod_lfo_phase_ += kTwoPi * static_cast<double>(kVibratoHz) * chunk / sr;
        if (mod_lfo_phase_ > kTwoPi) mod_lfo_phase_ -= kTwoPi;
        const float vib = mod * kVibratoSemis * static_cast<float>(std::sin(mod_lfo_phase_));
        const float semis = bend * kBendRangeSemis + vib;
        return (semis == 0.0f) ? 1.0f : std::pow(2.0f, semis / 12.0f);
    }

    void render_active_voices(audio::BufferView<float>& output, std::uint32_t start_frame,
                              std::uint32_t frames, const RenderParams& params) noexcept {
        if (frames == 0) return;
        const auto out_ch = std::min<std::uint32_t>(
            {static_cast<std::uint32_t>(output.num_channels()), prepared_output_channels_, kMaxOutputChannels});
        if (out_ch == 0) return;
        for (std::uint32_t ch = 0; ch < out_ch; ++ch) voice_scratch_ptrs_[ch] = voice_scratch_[ch].data();

        // Pitch-bend + mod-wheel vibrato map to a playback-rate multiplier on the
        // (tempo-matched, native-rate-1.0) buffer. Sampled per chunk: the LFO
        // advances by the chunk length so a held note's vibrato is continuous
        // across chunks/blocks. bend and mod are read once per chunk (relaxed).
        std::uint32_t rendered = 0;
        while (rendered < frames) {
            const auto chunk = std::min(frames - rendered, max_block_frames_);
            const float rate = apply_expression_rate(chunk);
            audio::BufferView<float> scratch(voice_scratch_ptrs_.data(), out_ch, chunk);
            for (auto& voice : voices_) {
                if (!voice.active) continue;
                std::array<const float*, kMaxSampleChannels> sptrs{};
                if (!store_.populate_channel_ptrs(voice.sample, sptrs.data(), sptrs.size())) { voice.reset(); continue; }
                audio::BufferView<const float> source(sptrs.data(), voice.sample.num_channels,
                                                      static_cast<std::size_t>(voice.sample.num_frames));
                voice.adsr.set_params(params.adsr);
                voice.renderer.set_playback_rate(rate);
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
        const auto region = region_for_note(note, sample, params.loop, params.play_to_end, params.loop_mode);
        if (!region) return;  // note maps to no slice -> silent (don't play the whole sample)
        SamplerVoice* target = nullptr;
        for (auto& voice : voices_) if (!voice.active) { target = &voice; break; }
        if (target == nullptr) target = &voices_[0];
        // Buffer is already at host tempo -> play at native rate (1.0).
        target->start(note, velocity, 1.0, host_sample_rate_, sample, *region, sample.num_frames);
        last_note_ = note;  // most-recently-triggered note drives the playhead
        if (note >= 0 && note < 128) held_velocity_[static_cast<size_t>(note)] = velocity;
    }

    void release_note(int note) {
        if (note >= 0 && note < 128) held_velocity_[static_cast<size_t>(note)] = 0.0f;  // key up -> not held
        // Gate off (One-Shot): key-up does NOT release — the slice plays through
        // (and a looping voice keeps looping until LOOP is turned off).
        if (state().get_value(kGate) < 0.5f) return;
        const bool hold = sustain_pedal_.load(std::memory_order_relaxed);
        for (auto& voice : voices_)
            if (voice.active && voice.note == note && !voice.released) {
                if (hold) voice.sustained = true;  // damper down: defer until the pedal lifts
                else voice.release();
            }
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

    // MIDI expression (UI keyboard buttons + host CC/pitch-wheel) -> audio thread.
    // RT-safe: UI/host threads store, the audio thread loads (relaxed). Applied in
    // render_active_voices as a per-voice playback-rate multiplier (bend + vibrato)
    // and a deferred note-off (sustain). See on_pitch_bend / on_modulation /
    // on_sustain wiring + apply_expression_rate().
    std::atomic<float> pitch_bend_{0.0f};     // -1..+1, scaled by kBendRangeSemis
    std::atomic<float> modulation_{0.0f};     // 0..1 vibrato depth
    std::atomic<bool> sustain_pedal_{false};  // damper hold
    static constexpr float kBendRangeSemis = 2.0f;   // +/- 2 semitones (Logic default)
    static constexpr float kVibratoSemis = 0.4f;     // peak vibrato at full mod
    static constexpr float kVibratoHz = 5.5f;        // vibrato LFO rate
    bool sustain_prev_ = false;   // audio-thread-only: pedal falling-edge detect
    bool loop_prev_ = false;      // audio-thread-only: LOOP-param edge detect
    // Velocity of each currently-held MIDI note (0 = not held), keyed by note-on/off
    // so a LOOP toggle can (re)engage looping on held keys. Audio-thread only.
    float held_velocity_[128] = {};
    double mod_lfo_phase_ = 0.0;  // audio-thread-only: vibrato LFO phase (radians)

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
    // Bumped ONLY when a genuinely new sample is loaded (load_loop), NOT on a
    // same-sample re-slice (reanalyze). The footer TEMPO poller watches this so a
    // slice-count change preserves the user's tempo override; raw_generation_ still
    // advances on re-slice so the waveform/slice display repaints. (Before this
    // split both shared raw_generation_, so changing slices snapped the tempo back
    // to the loop's detected BPM — the "slices reset my tempo" report.)
    std::atomic<std::uint64_t> load_generation_{0};
    std::atomic<double> pending_host_bpm_{120.0};
    // UI target-tempo override: 0 = follow host, else the BPM to stretch TO.
    std::atomic<double> tempo_override_{0.0};
    // Set by the editor: true when hosted in the standalone chrome (no DAW
    // transport), so process()/effective_bpm() resolve LINKED tempo to the loop's
    // natural BPM instead of a host tempo. Default false = behave as a plugin host.
    std::atomic<bool> standalone_host_{false};
    std::mutex drop_mutex_;
    std::string pending_drop_path_;

    // UI-thread -> audio-thread note injection (click/typing playback).
    struct UiNote { int note = -1; float velocity = 0.8f; bool on = false; };
    runtime::SpscQueue<UiNote, 256> ui_notes_;

    // Playhead: the audio thread publishes the MOST-RECENTLY-triggered active
    // voice's slice + progress; the UI draws a single playhead (Plunderphonics
    // style — only the latest shows, even if older voices still sound).
    std::atomic<int> ui_play_slice_{-1};
    std::atomic<float> ui_play_pos_{0.0f};  // 0..1 through the slice
    int last_note_ = -1;  // audio-thread only: most recent note_on

    // Musical-typing keyboard window (SDK primitive), lazily created on first ⌘K /
    // "Keyboard" toggle. UI-thread only. kb_ is declared BEFORE kb_window_ so the
    // window (which holds a View& to kb_) destructs first.
    std::unique_ptr<view::MusicalTypingKeyboard> kb_;
    std::unique_ptr<view::WindowHost> kb_window_;
    struct HeldKeyboardTrigger {
        int physical_note = 0;
        int trigger_note = 0;
    };
    // Currently-held keyboard notes (UI thread only). physical_note lights the
    // key; trigger_note is the note-on value to release even if ROOT changed.
    // Fed by QWERTY typing and keyboard clicks. Empty -> no lit keys.
    std::vector<HeldKeyboardTrigger> held_keyboard_triggers_ui_;
};

inline std::unique_ptr<format::Processor> create_pulp_tempo_sampler() {
    return std::make_unique<PulpTempoSamplerProcessor>();
}

} // namespace pulp::examples
