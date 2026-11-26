#include <pulp/vellum_audio/standalone_typing_panel.hpp>

#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace pulp::vellum_audio {

namespace {

namespace vv = ::vellum::view;

std::uint8_t midi_velocity(float velocity) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(velocity * 127.0f), 1l, 127l));
}

std::uint8_t midi_unit_value(float value) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(value * 127.0f), 0l, 127l));
}

std::uint16_t midi_pitch_bend(float bend) {
    const float unit = (std::clamp(bend, -1.0f, 1.0f) + 1.0f) * 0.5f;
    return static_cast<std::uint16_t>(std::clamp(std::lround(unit * 16383.0f), 0l, 16383l));
}

std::uint16_t platform_main_modifier() {
    return vv::is_main_modifier(vv::kModCmd) ? vv::kModCmd : vv::kModCtrl;
}

void restore_if_empty(std::atomic<std::uint64_t>& destination, std::uint64_t value) {
    if (value == 0)
        return;
    std::uint64_t empty = 0;
    destination.compare_exchange_strong(empty, value, std::memory_order_acq_rel);
}

// A window host that stands for a window the standalone owns: it carries the
// native handle so `position_beside` can place the keyboard next to it, and
// does nothing else.
class NativeWindowRef final : public vv::WindowHost {
  public:
    explicit NativeWindowRef(void* handle) : handle_(handle) {}
    void show() override {}
    void hide() override {}
    bool is_visible() const override {
        return true;
    }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
    void* native_window_handle() const override {
        return handle_;
    }

  private:
    void* handle_;
};

} // namespace

StandaloneTypingPanel::StandaloneTypingPanel(MidiSink midi_sink, HostFactory host_factory)
    : midi_sink_(std::move(midi_sink)), host_factory_(std::move(host_factory)) {
    if (!host_factory_) {
        host_factory_ = [](vv::View& root, const vv::WindowOptions& options) {
            return vv::WindowHost::create(root, options);
        };
    }
}

StandaloneTypingPanel::~StandaloneTypingPanel() {
    shutdown();
}

std::string StandaloneTypingPanel::menu_title() const {
    return "Musical Typing Keyboard";
}

char StandaloneTypingPanel::shortcut() const {
    return 'k';
}

bool StandaloneTypingPanel::toggle() {
    if (is_visible()) {
        hide();
        return true;
    }
    return show();
}

void StandaloneTypingPanel::set_primary_window(void* native_window) {
    primary_native_window_ = native_window;
}

bool StandaloneTypingPanel::emit(const midi::MidiEvent& event) {
    return midi_sink_ && midi_sink_(event);
}

void StandaloneTypingPanel::emit_sustain(bool enabled) {
    sustain_publication_.store(true, std::memory_order_release);
    const auto prior_generation = sustain_generation_.load(std::memory_order_acquire);
    sustain_generation_.store(prior_generation + 1, std::memory_order_release);
    if (emit(midi::MidiEvent::cc(0, 64, enabled ? 127 : 0))) {
        sustain_active_.store(enabled, std::memory_order_release);
        sustain_off_generation_.store(0, std::memory_order_release);
    } else {
        sustain_generation_.store(prior_generation, std::memory_order_release);
        if (!enabled && sustain_active_.load(std::memory_order_acquire))
            sustain_off_generation_.store(prior_generation, std::memory_order_release);
    }
    sustain_publication_.store(false, std::memory_order_release);
}

void StandaloneTypingPanel::emit_pitch_bend(float bend) {
    pitch_publication_.store(true, std::memory_order_release);
    const auto prior_generation = pitch_generation_.load(std::memory_order_acquire);
    pitch_generation_.store(prior_generation + 1, std::memory_order_release);
    if (emit(midi::MidiEvent::pitch_bend(0, midi_pitch_bend(bend)))) {
        pitch_active_.store(bend != 0.0f, std::memory_order_release);
        pitch_center_generation_.store(0, std::memory_order_release);
    } else {
        pitch_generation_.store(prior_generation, std::memory_order_release);
        if (bend == 0.0f && pitch_active_.load(std::memory_order_acquire))
            pitch_center_generation_.store(prior_generation, std::memory_order_release);
    }
    pitch_publication_.store(false, std::memory_order_release);
}

void StandaloneTypingPanel::request_note_off_recovery(std::uint8_t note) {
    note_recovery_generations_[note].store(note_generations_[note].load(std::memory_order_acquire),
                                           std::memory_order_release);
    auto& bits = note < 64 ? emergency_note_off_low_ : emergency_note_off_high_;
    bits.fetch_or(std::uint64_t{1} << (note & 63), std::memory_order_release);
}

void StandaloneTypingPanel::note_on(int note, float velocity) {
    if (note < 0 || note > 127)
        return;
    const auto midi_note = static_cast<std::uint8_t>(note);
    auto& holds = note_hold_counts_[midi_note];
    if (holds != std::numeric_limits<std::uint16_t>::max())
        ++holds;
    note_publications_[midi_note].store(true, std::memory_order_release);
    const auto prior_generation = note_generations_[midi_note].load(std::memory_order_acquire);
    note_generations_[midi_note].store(prior_generation + 1, std::memory_order_release);
    if (emit(midi::MidiEvent::note_on(0, midi_note, midi_velocity(velocity)))) {
        note_recovery_generations_[midi_note].store(0, std::memory_order_release);
    } else {
        note_generations_[midi_note].store(prior_generation, std::memory_order_release);
    }
    note_publications_[midi_note].store(false, std::memory_order_release);
}

void StandaloneTypingPanel::note_off(int note) {
    if (note < 0 || note > 127)
        return;
    const auto midi_note = static_cast<std::uint8_t>(note);
    auto& holds = note_hold_counts_[midi_note];
    if (holds == 0 || --holds != 0)
        return;
    if (!emit(midi::MidiEvent::note_off(0, midi_note)))
        request_note_off_recovery(midi_note);
}

void StandaloneTypingPanel::release_all_notes() {
    if (keyboard_) {
        keyboard_->set_input_capture(false);
        // Let the keyboard drop its OWN latched state (QWERTY-held notes, the
        // sustain hold, a deflected pitch bend) rather than only emitting the
        // MIDI for it. Emitting CC64=0 without clearing the view leaves the
        // sustain pad lit and `sustain_` true, so re-opening shows a control
        // that is on while the synth's is off, and the next press/release emits
        // a stray CC64=0 with no preceding CC64=127.
        //
        // This runs FIRST and routes back through our own on_note_off /
        // on_sustain / on_pitch_bend callbacks, so the residual sweep below
        // sees already-zeroed hold counts and cleared sustain/pitch flags and
        // does not emit anything twice.
        keyboard_->on_focus_changed(false);
    }

    for (std::size_t note = 0; note < note_hold_counts_.size(); ++note) {
        if (note_hold_counts_[note] == 0)
            continue;
        note_hold_counts_[note] = 0;
        if (!emit(midi::MidiEvent::note_off(0, static_cast<std::uint8_t>(note))))
            request_note_off_recovery(static_cast<std::uint8_t>(note));
    }

    if (sustain_active_.load(std::memory_order_acquire))
        emit_sustain(false);
    if (pitch_active_.load(std::memory_order_acquire))
        emit_pitch_bend(0.0f);
}

void StandaloneTypingPanel::drain_midi_into(midi::MidiBuffer& midi_in, int frame_count) noexcept {
    const int recovery_offset = std::max(0, frame_count - 1);
    auto drain_note_offs = [this, &midi_in, recovery_offset](std::atomic<std::uint64_t>& bits,
                                                             int base_note) {
        std::uint64_t pending = bits.exchange(0, std::memory_order_acq_rel);
        std::uint64_t retry = 0;
        while (pending != 0) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(pending));
            const auto note = static_cast<std::uint8_t>(base_note + bit);
            const auto recovery_generation =
                note_recovery_generations_[note].exchange(0, std::memory_order_acq_rel);
            if (note_publications_[note].load(std::memory_order_acquire)) {
                restore_if_empty(note_recovery_generations_[note], recovery_generation);
                retry |= std::uint64_t{1} << bit;
                pending &= pending - 1;
                continue;
            }
            if (recovery_generation == 0 ||
                note_generations_[note].load(std::memory_order_acquire) != recovery_generation) {
                pending &= pending - 1;
                continue;
            }
            auto event = midi::MidiEvent::note_off(0, note);
            event.sample_offset = recovery_offset;
            if (!midi_in.add(event)) {
                retry |= std::uint64_t{1} << bit;
                restore_if_empty(note_recovery_generations_[note], recovery_generation);
            }
            pending &= pending - 1;
        }
        if (retry != 0)
            bits.fetch_or(retry, std::memory_order_release);
    };
    drain_note_offs(emergency_note_off_low_, 0);
    drain_note_offs(emergency_note_off_high_, 64);

    const auto sustain_off_generation =
        sustain_off_generation_.exchange(0, std::memory_order_acq_rel);
    if (sustain_publication_.load(std::memory_order_acquire)) {
        restore_if_empty(sustain_off_generation_, sustain_off_generation);
    } else if (sustain_off_generation != 0 &&
               sustain_generation_.load(std::memory_order_acquire) == sustain_off_generation) {
        auto sustain_off = midi::MidiEvent::cc(0, 64, 0);
        sustain_off.sample_offset = recovery_offset;
        if (!midi_in.add(sustain_off)) {
            restore_if_empty(sustain_off_generation_, sustain_off_generation);
        } else {
            sustain_active_.store(false, std::memory_order_release);
        }
    }
    const auto pitch_center_generation =
        pitch_center_generation_.exchange(0, std::memory_order_acq_rel);
    if (pitch_publication_.load(std::memory_order_acquire)) {
        restore_if_empty(pitch_center_generation_, pitch_center_generation);
    } else if (pitch_center_generation != 0 &&
               pitch_generation_.load(std::memory_order_acquire) == pitch_center_generation) {
        auto pitch_center = midi::MidiEvent::pitch_bend(0, 8192);
        pitch_center.sample_offset = recovery_offset;
        if (!midi_in.add(pitch_center)) {
            restore_if_empty(pitch_center_generation_, pitch_center_generation);
        } else {
            pitch_active_.store(false, std::memory_order_release);
        }
    }
}

void StandaloneTypingPanel::create_keyboard() {
    if (keyboard_)
        return;
    keyboard_ = std::make_unique<MusicalTypingKeyboard>();
    keyboard_->set_input_capture(true);
    keyboard_->on_note_on = [this](int note, float velocity) { note_on(note, velocity); };
    keyboard_->on_note_off = [this](int note) { note_off(note); };
    keyboard_->on_pitch_bend = [this](float bend) { emit_pitch_bend(bend); };
    keyboard_->on_modulation = [this](float amount) {
        emit(midi::MidiEvent::cc(0, 1, midi_unit_value(amount)));
    };
    keyboard_->on_sustain = [this](bool enabled) { emit_sustain(enabled); };
    keyboard_->on_global_key = [this](const vv::KeyEvent& event) {
        if (!event.is_down || event.is_repeat)
            return false;
        const bool main_chord = event.modifiers == platform_main_modifier();
        const bool should_hide =
            (main_chord && (event.key == vv::KeyCode::k || event.key == vv::KeyCode::w)) ||
            (event.modifiers == vv::kModNone && event.key == vv::KeyCode::escape);
        if (!should_hide)
            return false;
        hide();
        return true;
    };
}

bool StandaloneTypingPanel::show() {
    create_keyboard();
    if (!window_) {
        const float width = keyboard_->panel_width();
        const float height = keyboard_->panel_height();
        vv::WindowOptions options;
        options.title = "Musical Typing Keyboard";
        options.use_gpu = true;
        options.secondary_window = true;
        options.width = width;
        options.height = height;
        options.min_width = width;
        options.min_height = 176.0f;
        options.resizable = true;
        window_ = host_factory_(*keyboard_, options);
        if (!window_ || !window_->is_gpu_backed()) {
            // The keyboard paints through Skia, so a CPU-only host cannot show
            // it. Say so: the menu item and the shortcut are already installed
            // by now, and a silent bail-out reads as a dead menu item.
            runtime::log_warn(
                "[musical-typing] cannot open: this build's WindowHost is not GPU-backed "
                "(Skia/Dawn unavailable). The Musical Typing Keyboard command "
                "will not work.");
            window_.reset();
            keyboard_.reset();
            return false;
        }
        window_->set_design_viewport(width, height);
        window_->set_fixed_aspect_ratio(width / height);
        auto* host = window_.get();
        keyboard_->on_intrinsic_size_changed = [host](float w, float h) {
            host->set_fixed_aspect_ratio(w / h);
            host->set_design_viewport(w, h);
            host->request_content_size(w, h);
        };
        window_->set_close_callback([this] {
            release_all_notes();
            if (window_)
                window_->set_app_key_monitor({});
        });
    }

    keyboard_->set_input_capture(true);
    window_->show();
    if (primary_native_window_) {
        NativeWindowRef primary(primary_native_window_);
        window_->position_beside(&primary);
    }
    window_->set_app_key_monitor(
        [this](const vv::KeyEvent& event) { return route_app_key(event); });
    return true;
}

void StandaloneTypingPanel::hide() {
    if (!keyboard_ && !window_)
        return;
    release_all_notes();
    if (!window_)
        return;
    window_->set_app_key_monitor({});
    window_->hide();
}

void StandaloneTypingPanel::shutdown() noexcept {
    hide();
    if (window_)
        window_->set_close_callback({});
    window_.reset();
    keyboard_.reset();
    primary_native_window_ = nullptr;
    note_hold_counts_.fill(0);
}

bool StandaloneTypingPanel::is_visible() const {
    return window_ && window_->is_visible();
}

bool StandaloneTypingPanel::route_app_key(const vv::KeyEvent& event) {
    if (!keyboard_ || !is_visible())
        return false;
    if (!event.is_down) {
        auto release = event;
        release.modifiers = vv::kModNone;
        return keyboard_->on_key_event(release);
    }
    auto* focused = keyboard_->interaction().focused_input;
    if (focused && focused->accepts_text_input())
        return false;
    if ((event.modifiers & (vv::kModCmd | vv::kModCtrl | vv::kModAlt | vv::kModMeta)) != 0 ||
        event.key == vv::KeyCode::escape) {
        return false;
    }
    return keyboard_->on_key_event(event);
}

std::unique_ptr<format::StandalonePanel>
make_standalone_typing_panel(const format::StandalonePanelContext& context) {
    if (!context.config.enable_musical_typing_keyboard)
        return nullptr;
    return std::make_unique<StandaloneTypingPanel>(context.midi_sink);
}

bool install_standalone_typing_panel() noexcept {
    return format::detail::install_standalone_panel_factory(&make_standalone_typing_panel);
}

} // namespace pulp::vellum_audio
