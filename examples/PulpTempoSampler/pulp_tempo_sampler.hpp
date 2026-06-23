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
#include <pulp/view/buttons.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/musical_typing.hpp>
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
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
    audio::LoopRegion region;   // stored so the UI playhead can map position->progress
    bool released = false;

    void reset() {
        active = false; note = -1; velocity = 0.0f; sample = {}; released = false;
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

// Compact interactive musical-typing keyboard, Ink & Signal styled. Its 16 keys
// ARE the QWERTY musical-typing map (slice idx = note - root): white keys
// A S D F G H J K L, black keys W E T Y U O P, ~1.25 octave C..D#. Click a key
// to audition that slice; the key under the playhead lights up, so typing, MIDI
// and clicks all show. Toggled by ⌘K. This is the interactive companion to
// MusicalTypingController; the faithful Figma SVG component stays the design
// reference until it is re-exported with per-key interactive elements.
class InteractiveKeyboard : public view::View {
public:
    std::function<void(int slice, bool on)> on_play_slice;  // click -> audition
    std::function<int()> active_slice;                      // playhead slice or -1
    std::function<std::string()> root_label;                // e.g. "C3"

    InteractiveKeyboard() { set_continuous_repaint(true); }

    void on_mouse_event(const view::MouseEvent& e) override {
        if (e.is_wheel) return;
        const bool ph = e.hasExplicitPhase();
        const bool down = ph ? e.isPress() : e.is_down;
        const bool up   = ph ? e.isRelease() : !e.is_down;
        if (down && e.button == view::MouseButton::left) {
            const int s = key_at(e.position.x, e.position.y);
            if (s >= 0) { pressed_ = s; if (on_play_slice) on_play_slice(s, true); }
        } else if (up && pressed_ >= 0) {
            if (on_play_slice) on_play_slice(pressed_, false);
            pressed_ = -1;
        }
    }

    void paint(canvas::Canvas& c) override {
        using canvas::Color;
        auto b = local_bounds();
        const Color panel  = Color::rgba8(0x1E, 0x25, 0x30);
        const Color border = Color::rgba8(0x39, 0x41, 0x4A);
        const Color wkey   = Color::rgba8(0xE9, 0xED, 0xF2);
        const Color bkey   = Color::rgba8(0x2A, 0x32, 0x3D);
        const Color teal   = Color::rgba8(0x16, 0xDA, 0xC2);
        const Color ink    = Color::rgba8(0x16, 0x1A, 0x21);  // dark text (on light/active)
        const Color wtxt   = Color::rgba8(0xD6, 0xDC, 0xE4);  // light text (on black keys)
        const Color muted  = Color::rgba8(0x93, 0x9C, 0xA9);

        c.set_fill_color(panel);
        c.fill_rounded_rect(b.x, b.y, b.width, b.height, 12.f);
        c.set_stroke_color(border); c.set_line_width(1.f);
        c.stroke_rounded_rect(b.x, b.y, b.width, b.height, 12.f);

        c.set_fill_color(muted);
        c.set_font("JetBrains Mono", 11.f);
        c.set_text_align(canvas::TextAlign::left);
        c.fill_text("MUSICAL TYPING", b.x + 16.f, b.y + 24.f);
        if (root_label) {
            c.set_text_align(canvas::TextAlign::right);
            c.fill_text("ROOT " + root_label(), b.x + b.width - 16.f, b.y + 24.f);
        }

        const int act = active_slice ? active_slice() : -1;
        auto hot = [&](int i){ return i == pressed_ || i == act; };

        // White keys, then black keys on top.
        for (int i = 0; i < kNumKeys; ++i) {
            if (is_black(i)) continue;
            KRect r = white_rect(i);
            c.set_fill_color(hot(i) ? teal : wkey);
            c.fill_rounded_rect(r.x + 1.f, r.y, r.w - 2.f, r.h, 4.f);
            c.set_fill_color(ink);
            c.set_font("JetBrains Mono", 13.f);
            c.set_text_align(canvas::TextAlign::center);
            c.fill_text(letter(i), r.x + r.w * 0.5f, r.y + r.h - 12.f);
        }
        for (int i = 0; i < kNumKeys; ++i) {
            if (!is_black(i)) continue;
            KRect r = black_rect(i);
            c.set_fill_color(hot(i) ? teal : bkey);
            c.fill_rounded_rect(r.x, r.y, r.w, r.h, 3.f);
            c.set_fill_color(hot(i) ? ink : wtxt);
            c.set_font("JetBrains Mono", 11.f);
            c.set_text_align(canvas::TextAlign::center);
            c.fill_text(letter(i), r.x + r.w * 0.5f, r.y + r.h - 8.f);
        }
        c.set_text_align(canvas::TextAlign::left);
    }

private:
    struct KRect { float x, y, w, h; };
    static constexpr int kNumKeys = 16;

    static const char* letter(int i) {
        static const char* const L[16] =
            {"A","W","S","E","D","F","T","G","Y","H","U","J","K","O","L","P"};
        return (i >= 0 && i < 16) ? L[i] : "";
    }
    static bool is_black(int i) {
        switch (i) {
            case 1: case 3: case 6: case 8: case 10: case 13: case 15: return true;
            default: return false;
        }
    }
    static int white_count() { return 9; }
    int white_index(int sliceidx) const {
        int wi = 0; for (int i = 0; i < sliceidx; ++i) if (!is_black(i)) ++wi; return wi;
    }
    KRect keyarea() const {
        auto b = local_bounds();
        const float padx = 16.f, top = 40.f, bot = 16.f;
        return {b.x + padx, b.y + top, b.width - 2 * padx, b.height - top - bot};
    }
    KRect white_rect(int sliceidx) const {
        KRect g = keyarea(); float ww = g.w / static_cast<float>(white_count());
        return {g.x + static_cast<float>(white_index(sliceidx)) * ww, g.y, ww, g.h};
    }
    KRect black_rect(int sliceidx) const {
        KRect g = keyarea(); float ww = g.w / static_cast<float>(white_count());
        const int wi = white_index(sliceidx - 1);  // sliceidx-1 is the preceding white
        const float bw = ww * 0.6f, bh = g.h * 0.6f;
        return {g.x + static_cast<float>(wi + 1) * ww - bw * 0.5f, g.y, bw, bh};
    }
    static bool inside(const KRect& r, float x, float y) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    }
    int key_at(float x, float y) const {
        for (int i = 0; i < kNumKeys; ++i)
            if (is_black(i)) { KRect r = black_rect(i); if (inside(r, x, y)) return i; }
        for (int i = 0; i < kNumKeys; ++i)
            if (!is_black(i)) { KRect r = white_rect(i); if (inside(r, x, y)) return i; }
        return -1;
    }
    int pressed_ = -1;
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
    // The on-screen interactive musical-typing keyboard. Hidden by default;
    // ⌘K (Ctrl+K on Win/Linux) toggles it. Click keys to audition slices; the
    // playhead key lights up so typing / MIDI / clicks all show.
    InteractiveKeyboard* keyboard = nullptr;

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

    // Show/hide the on-screen keyboard. Returns the new visibility.
    bool toggle_keyboard() {
        if (!keyboard) return false;
        const bool show = !keyboard->visible();
        keyboard->set_visible(show);
        return show;
    }

private:
    bool on_musical_key(const view::KeyEvent& e) {
        // ⌘K (macOS) / Ctrl+K (Win/Linux): toggle the on-screen keyboard. Checked
        // before the modifier-chord rejection below so the command-combo reaches
        // here (the macOS host routes Cmd chords via performKeyEquivalent:).
        const uint16_t toggle_mod = view::kModCmd | view::kModCtrl;
        if (e.key == view::KeyCode::k && (e.modifiers & toggle_mod) && e.is_down && !e.is_repeat) {
            toggle_keyboard();
            return true;
        }
        // Don't turn typing into a name field into notes — if a text-input widget
        // owns focus, let it have the keys.
        if (auto* fv = view::View::focused_input_; fv && fv->accepts_text_input())
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
    static constexpr int kDefaultRootNote = 60;  // C3 in C-2..C8 labeling (see note_name)
    static constexpr int kRootNoteMin = 0;       // C-2
    static constexpr int kRootNoteMax = 120;     // C8

    // Onset sensitivity s in [0,1] maps to a slice COUNT (the strongest cuts by
    // confidence are kept): s=0 -> 1 slice (whole sample), s=1 -> kMaxSlices.
    static constexpr int kMaxSlices = 32;
    static constexpr float kDefaultOnsetSensitivity = 0.5f;  // ~16 slices by default

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
            .version = "1.0.2",
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

    // ── Plugin state: persist the loaded sample across reload / project save ──
    // The host calls these on save/restore; we serialize the raw loop audio so a
    // dropped sample survives closing+reopening the editor, plugin reload, and
    // DAW project save. (The slices/tempo re-derive on load.)
    std::vector<std::uint8_t> serialize_plugin_state() const override {
        std::lock_guard<std::mutex> lock(raw_mutex_);
        std::vector<std::uint8_t> out;
        if (raw_frames_ <= 0) return out;  // nothing loaded
        const std::uint32_t magic = 0x504C5053u;  // 'PLPS'
        const std::uint32_t version = 1;
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
        load_loop(ptrs.data(), ch, static_cast<long>(frames), sr);
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
            auto picked = platform::FileDialog::open_file(
                "Load a sample",
                {{"Audio Files", "wav;aif;aiff;flac;mp3;ogg"}}, "");
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
        // into notes; base = ROOT so 'a' plays slice 0, 'w' slice 1, … Notes go
        // to the same lock-free UI->audio queue as host MIDI and slice clicks.
        root->current_root_note = [this] { return static_cast<int>(state().get_value(kRootNote)); };
        root->typing.on_note_on = [this](int note, float vel) { ui_note_on(note, vel); };
        root->typing.on_note_off = [this](int note) { ui_note_off(note); };

        // ── Footer: wired controls only ──
        // Root-note dropdown (slice 0 maps to this MIDI note; idx = note - root).
        // Labels follow the requested C-2..C8 convention (C3 = 60); see note_name.
        // Root = octave (C-2..C8). Octave granularity keeps the dropdown short
        // enough to fit + flip above when needed (a 121-note chromatic list was
        // taller than the window and got clipped). idx -> MIDI note idx*12.
        label(20, 320, 40, 18, "ROOT", faint, 10, 600, LabelAlign::left, true);
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

        // LOOP toggle: enable/disable Forward looping for triggered slices
        // (one-shot when off). Bound to kTempoLoop; region_for_note reads it per
        // note (playback_mode = loop ? Forward : OneShot). Top-level for now;
        // per-slice loop / reverse are a future UX.
        {
            auto loopBtn = std::make_unique<ToggleButton>();
            loopBtn->set_label("LOOP");
            loopBtn->set_font_size(10.0f);
            loopBtn->set_corner_radius(6.0f);
            loopBtn->set_on_background_color(teal);
            loopBtn->set_on_text_color(bg900);
            loopBtn->set_off_background_color(raised);
            loopBtn->set_off_text_color(muted);
            loopBtn->set_off_border_color(faint);
            place(*loopBtn, 556, 316, 84, 26);
            root->bindings.push_back(bind_parameter(*loopBtn, state(), kTempoLoop));
            root->add_child(std::move(loopBtn));
        }

        // On-screen interactive musical-typing keyboard. Hidden until ⌘K/Ctrl+K.
        // Added LAST so it paints on top as a floating panel. Click a key to
        // audition its slice (same path as a waveform-slice tap); the playhead
        // key lights up so typing, MIDI and clicks all read on it.
        {
            auto kbd = std::make_unique<InteractiveKeyboard>();
            place(*kbd, 60, 86, 640, 196);
            kbd->set_visible(false);
            kbd->on_play_slice = [this](int idx, bool on) { play_slice(idx, on); };
            kbd->active_slice = [this] { int s = -1; float p = 0.0f; playhead(s, p); return s; };
            kbd->root_label = [this] { return note_name(static_cast<int>(state().get_value(kRootNote))); };
            root->keyboard = kbd.get();
            root->add_child(std::move(kbd));
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
        publish_playhead();
        publish_audio_acknowledgement(published);
    }

private:
    struct RenderParams {
        float gain = 1.0f;
        signal::Adsr::Params adsr;
        bool loop = false;
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

        std::vector<audio::OnsetMarker> cand;
        for (const auto& m : onsets.markers)
            if (static_cast<long>(m.frame) > 0 && static_cast<long>(m.frame) < raw_frames_)
                cand.push_back(m);
        std::sort(cand.begin(), cand.end(),
                  [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
        if (static_cast<int>(cand.size()) > keep) cand.resize(static_cast<std::size_t>(keep));

        slices_orig_.clear();
        slices_orig_.push_back(0);
        for (const auto& m : cand) slices_orig_.push_back(static_cast<long>(m.frame));
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

        // Refresh the slice boundaries into the stretched buffer FIRST, before the
        // audio re-publish below. A sensitivity-only re-slice keeps the same
        // time-ratio + audio, so these boundaries are valid for the CURRENT
        // published buffer — and updating here means the keyboard trigger mapping
        // follows the SENS slider even when the re-publish is skipped because a
        // held voice still occupies the store slot (the bug: `if (!ok) return`
        // used to bail before this update). region_for_note clamps any boundary
        // past the live buffer length to silence, so a deferred-publish tempo
        // change degrades safely rather than reading stale regions.
        {
            std::vector<long> scaled;
            scaled.reserve(slices.size());
            for (long s : slices)
                scaled.push_back(std::min<long>(out_frames, static_cast<long>(std::llround(s * R))));
            std::lock_guard<std::mutex> lock(slice_mutex_);
            slices_stretched_ = std::move(scaled);
        }

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
        // slices_stretched_ already refreshed above (before the publish), so a
        // skipped re-publish never leaves the trigger mapping stale.
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
    std::optional<audio::LoopRegion> region_for_note(int note, const audio::PublishedSampleView& sample,
                                                     bool loop) const noexcept {
        std::uint64_t start = 0, end = 0;
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
        const auto region = region_for_note(note, sample, params.loop);
        if (!region) return;  // note maps to no slice -> silent (don't play the whole sample)
        SamplerVoice* target = nullptr;
        for (auto& voice : voices_) if (!voice.active) { target = &voice; break; }
        if (target == nullptr) target = &voices_[0];
        // Buffer is already at host tempo -> play at native rate (1.0).
        target->start(note, velocity, 1.0, host_sample_rate_, sample, *region, sample.num_frames);
        last_note_ = note;  // most-recently-triggered note drives the playhead
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

    // Playhead: the audio thread publishes the MOST-RECENTLY-triggered active
    // voice's slice + progress; the UI draws a single playhead (Plunderphonics
    // style — only the latest shows, even if older voices still sound).
    std::atomic<int> ui_play_slice_{-1};
    std::atomic<float> ui_play_pos_{0.0f};  // 0..1 through the slice
    int last_note_ = -1;  // audio-thread only: most recent note_on
};

inline std::unique_ptr<format::Processor> create_pulp_tempo_sampler() {
    return std::make_unique<PulpTempoSamplerProcessor>();
}

} // namespace pulp::examples
