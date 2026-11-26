#pragma once

/// @file standalone_typing_panel.hpp
/// The musical typing keyboard as a standalone tool window drawn by Vellum.
///
/// `StandaloneTypingPanel` implements `format::StandalonePanel`: the standalone
/// owns the menu item and the Cmd/Ctrl+K shortcut, and this panel owns the
/// keyboard window and the MIDI it produces. Notes, sustain, pitch bend and
/// modulation go to the standalone's UI MIDI queue; an event the queue refuses
/// (a note-off that found it full) is kept and handed to the audio thread
/// through `drain_midi_into`, so a stuck note is never the cost of a full queue.

#include <pulp/format/standalone_panel.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/vellum_audio/musical_typing_keyboard.hpp>

#include <vellum/view/window_host.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::vellum_audio {

class StandaloneTypingPanel final : public format::StandalonePanel {
  public:
    using MidiSink = std::function<bool(const midi::MidiEvent&)>;
    using HostFactory = std::function<std::unique_ptr<::vellum::view::WindowHost>(
        ::vellum::view::View&, const ::vellum::view::WindowOptions&)>;

    /// `host_factory` defaults to `WindowHost::create`; tests pass their own.
    /// Answers whether an editable text field anywhere in the app has keyboard
    /// focus; the panel then leaves typed keys to the field. Empty: never.
    using TextFocusProbe = std::function<bool()>;

    explicit StandaloneTypingPanel(MidiSink midi_sink, HostFactory host_factory = {},
                                   TextFocusProbe text_focus = {});
    ~StandaloneTypingPanel() override;

    StandaloneTypingPanel(const StandaloneTypingPanel&) = delete;
    StandaloneTypingPanel& operator=(const StandaloneTypingPanel&) = delete;

    std::string menu_title() const override;
    char shortcut() const override;
    bool toggle() override;
    bool is_visible() const override;
    void set_primary_window(void* native_window) override;
    void drain_midi_into(midi::MidiBuffer& midi, int frame_count) noexcept override;
    void shutdown() noexcept override;

    /// Open the window. False when the window host cannot draw the keyboard.
    bool show();
    void hide();

    MusicalTypingKeyboard* keyboard() {
        return keyboard_.get();
    }
    ::vellum::view::WindowHost* window() {
        return window_.get();
    }
    /// Offer a key to the panel the way the window's app-wide key monitor does.
    bool route_app_key(const ::vellum::view::KeyEvent& event);

  private:
    bool emit(const midi::MidiEvent& event);
    void emit_sustain(bool enabled);
    void emit_pitch_bend(float bend);
    void request_note_off_recovery(std::uint8_t note);
    void note_on(int note, float velocity);
    void note_off(int note);
    void release_all_notes();
    void create_keyboard();

    MidiSink midi_sink_;
    HostFactory host_factory_;
    TextFocusProbe text_focus_;
    void* primary_native_window_ = nullptr;
    std::unique_ptr<MusicalTypingKeyboard> keyboard_;
    std::unique_ptr<::vellum::view::WindowHost> window_;
    std::array<std::uint16_t, 128> note_hold_counts_{};
    std::array<std::atomic<std::uint64_t>, 128> note_generations_{};
    std::array<std::atomic<std::uint64_t>, 128> note_recovery_generations_{};
    std::array<std::atomic<bool>, 128> note_publications_{};
    std::atomic<std::uint64_t> emergency_note_off_low_{0};
    std::atomic<std::uint64_t> emergency_note_off_high_{0};
    std::atomic<std::uint64_t> sustain_generation_{1};
    std::atomic<std::uint64_t> sustain_off_generation_{0};
    std::atomic<bool> sustain_publication_{false};
    std::atomic<bool> sustain_active_{false};
    std::atomic<std::uint64_t> pitch_generation_{1};
    std::atomic<std::uint64_t> pitch_center_generation_{0};
    std::atomic<bool> pitch_publication_{false};
    std::atomic<bool> pitch_active_{false};
};

/// The factory the standalone calls for each panel slot. Returns null unless
/// the config sets `enable_musical_typing_keyboard`.
std::unique_ptr<format::StandalonePanel>
make_standalone_typing_panel(const format::StandalonePanelContext& context);

/// Install `make_standalone_typing_panel` for this executable. Call it from a
/// source compiled into the standalone (a static-init object in this archive
/// would be dropped by the linker). Installing twice is a no-op.
bool install_standalone_typing_panel() noexcept;

} // namespace pulp::vellum_audio
