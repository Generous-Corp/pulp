#include <pulp/format/detail/standalone_musical_typing.hpp>

#include <pulp/view/view.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace pulp::format::detail {

namespace {

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
    return view::is_main_modifier(view::kModCmd) ? view::kModCmd : view::kModCtrl;
}

void restore_if_empty(std::atomic<std::uint64_t>& destination, std::uint64_t value) {
    if (value == 0)
        return;
    std::uint64_t empty = 0;
    destination.compare_exchange_strong(empty, value, std::memory_order_acq_rel);
}

} // namespace

struct StandaloneMusicalTyping::CallbackState {
    StandaloneMusicalTyping* owner = nullptr;
    std::function<bool(const view::KeyEvent&)> prior_key_route;
};

StandaloneMusicalTyping::StandaloneMusicalTyping(MidiSink midi_sink, HostFactory host_factory)
    : midi_sink_(std::move(midi_sink)), host_factory_(std::move(host_factory)),
      callback_state_(std::make_shared<CallbackState>()) {
    callback_state_->owner = this;
    if (!host_factory_) {
        host_factory_ = [](view::View& root, const view::WindowOptions& options) {
            return view::WindowHost::create(root, options);
        };
    }
}

StandaloneMusicalTyping::~StandaloneMusicalTyping() {
    shutdown();
    if (registry_)
        registry_->remove_handler(this);
}

void StandaloneMusicalTyping::register_command(view::CommandRegistry& registry) {
    if (registry_ == &registry)
        return;
    if (registry_)
        registry_->remove_handler(this);
    registry_ = &registry;
    registry_->register_command({
        .id = kToggleStandaloneMusicalTypingCommand,
        .name = "Musical Typing Keyboard",
        .category = "Window",
        .enabled = true,
    });
    registry_->add_handler(this);
}

void StandaloneMusicalTyping::install_key_route(view::View& root) {
    if (routed_root_) {
        routed_root_->on_global_key = std::move(callback_state_->prior_key_route);
        routed_root_ = nullptr;
    }
    routed_root_ = &root;
    callback_state_->prior_key_route = std::move(root.on_global_key);
    std::weak_ptr<CallbackState> weak_state = callback_state_;
    root.on_global_key = [weak_state](const view::KeyEvent& event) {
        auto state = weak_state.lock();
        if (!state)
            return false;
        auto* owner = state->owner;
        if (event.is_down && !event.is_repeat && event.key == view::KeyCode::k &&
            event.modifiers == platform_main_modifier()) {
            return owner && owner->registry_ &&
                   owner->registry_->dispatch(kToggleStandaloneMusicalTypingCommand);
        }
        return state->prior_key_route ? state->prior_key_route(event) : false;
    };
}

void StandaloneMusicalTyping::add_menu_command(view::WindowOptions& options) {
    std::weak_ptr<CallbackState> weak_state = callback_state_;
    options.menu_commands.push_back({
        .menu = "Window",
        .title = "Musical Typing Keyboard",
        .key = view::KeyCode::k,
        .modifiers = platform_main_modifier(),
        .action =
            [weak_state] {
                auto state = weak_state.lock();
                if (state && state->owner && state->owner->registry_) {
                    state->owner->registry_->dispatch(kToggleStandaloneMusicalTypingCommand);
                }
            },
    });
}

std::vector<view::CommandID> StandaloneMusicalTyping::commands() const {
    return {kToggleStandaloneMusicalTypingCommand};
}

bool StandaloneMusicalTyping::perform_command(view::CommandID id) {
    if (id != kToggleStandaloneMusicalTypingCommand)
        return false;
    toggle();
    return true;
}

bool StandaloneMusicalTyping::emit(const midi::MidiEvent& event) {
    return midi_sink_ && midi_sink_(event);
}

void StandaloneMusicalTyping::emit_sustain(bool enabled) {
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

void StandaloneMusicalTyping::emit_pitch_bend(float bend) {
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

void StandaloneMusicalTyping::request_note_off_recovery(std::uint8_t note) {
    note_recovery_generations_[note].store(note_generations_[note].load(std::memory_order_acquire),
                                           std::memory_order_release);
    auto& bits = note < 64 ? emergency_note_off_low_ : emergency_note_off_high_;
    bits.fetch_or(std::uint64_t{1} << (note & 63), std::memory_order_release);
}

void StandaloneMusicalTyping::note_on(int note, float velocity) {
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

void StandaloneMusicalTyping::note_off(int note) {
    if (note < 0 || note > 127)
        return;
    const auto midi_note = static_cast<std::uint8_t>(note);
    auto& holds = note_hold_counts_[midi_note];
    if (holds == 0 || --holds != 0)
        return;
    if (!emit(midi::MidiEvent::note_off(0, midi_note)))
        request_note_off_recovery(midi_note);
}

void StandaloneMusicalTyping::release_all_notes() {
    if (keyboard_)
        keyboard_->set_input_capture(false);

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

void StandaloneMusicalTyping::drain_recovery_into(midi::MidiBuffer& midi_in,
                                                  int frame_count) noexcept {
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

void StandaloneMusicalTyping::create_keyboard() {
    if (keyboard_)
        return;
    keyboard_ = std::make_unique<view::MusicalTypingKeyboard>();
    keyboard_->set_input_capture(true);
    keyboard_->on_note_on = [this](int note, float velocity) { note_on(note, velocity); };
    keyboard_->on_note_off = [this](int note) { note_off(note); };
    keyboard_->on_pitch_bend = [this](float bend) { emit_pitch_bend(bend); };
    keyboard_->on_modulation = [this](float amount) {
        emit(midi::MidiEvent::cc(0, 1, midi_unit_value(amount)));
    };
    keyboard_->on_sustain = [this](bool enabled) { emit_sustain(enabled); };
    keyboard_->on_global_key = [this](const view::KeyEvent& event) {
        if (!event.is_down || event.is_repeat)
            return false;
        const bool main_chord = event.modifiers == platform_main_modifier();
        const bool should_hide =
            (main_chord && (event.key == view::KeyCode::k || event.key == view::KeyCode::w)) ||
            (event.modifiers == view::kModNone && event.key == view::KeyCode::escape);
        if (!should_hide)
            return false;
        hide();
        return true;
    };
}

bool StandaloneMusicalTyping::show() {
    create_keyboard();
    if (!window_) {
        const float width = keyboard_->panel_width();
        const float height = keyboard_->panel_height();
        view::WindowOptions options;
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
    window_->position_beside(primary_window_);
    window_->set_app_key_monitor(
        [this](const view::KeyEvent& event) { return route_app_key(event); });
    return true;
}

void StandaloneMusicalTyping::hide() {
    if (!keyboard_ && !window_)
        return;
    release_all_notes();
    if (!window_)
        return;
    window_->set_app_key_monitor({});
    window_->hide();
}

void StandaloneMusicalTyping::toggle() {
    if (is_visible()) {
        hide();
    } else {
        show();
    }
}

void StandaloneMusicalTyping::shutdown() {
    hide();
    if (window_)
        window_->set_close_callback({});
    window_.reset();
    keyboard_.reset();
    primary_window_ = nullptr;
    note_hold_counts_.fill(0);
    if (routed_root_) {
        routed_root_->on_global_key = std::move(callback_state_->prior_key_route);
        routed_root_ = nullptr;
    }
    callback_state_->owner = nullptr;
}

bool StandaloneMusicalTyping::is_visible() const {
    return window_ && window_->is_visible();
}

bool StandaloneMusicalTyping::route_app_key(const view::KeyEvent& event) {
    if (!keyboard_ || !is_visible())
        return false;
    if (!event.is_down) {
        auto release = event;
        release.modifiers = view::kModNone;
        return keyboard_->on_key_event(release);
    }
    if (auto* focused = view::View::focused_input_; focused && focused->accepts_text_input())
        return false;
    if ((event.modifiers & (view::kModCmd | view::kModCtrl | view::kModAlt | view::kModMeta)) !=
            0 ||
        event.key == view::KeyCode::escape) {
        return false;
    }
    return keyboard_->on_key_event(event);
}

} // namespace pulp::format::detail
