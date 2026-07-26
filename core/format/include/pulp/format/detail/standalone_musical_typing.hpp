#pragma once

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/view/command_registry.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/window_host.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::format::detail {

inline constexpr view::CommandID kToggleStandaloneMusicalTypingCommand = 0x50554C4Bu;

class StandaloneMusicalTyping final : public view::CommandHandler {
  public:
    using MidiSink = std::function<bool(const midi::MidiEvent&)>;
    using HostFactory =
        std::function<std::unique_ptr<view::WindowHost>(view::View&, const view::WindowOptions&)>;

    explicit StandaloneMusicalTyping(MidiSink midi_sink, HostFactory host_factory = {});
    ~StandaloneMusicalTyping() override;

    StandaloneMusicalTyping(const StandaloneMusicalTyping&) = delete;
    StandaloneMusicalTyping& operator=(const StandaloneMusicalTyping&) = delete;

    void register_command(view::CommandRegistry& registry);
    void install_key_route(view::View& root);
    void add_menu_command(view::WindowOptions& options);
    void set_primary_window(view::WindowHost* primary) {
        primary_window_ = primary;
    }

    bool show();
    void hide();
    void toggle();
    void shutdown();
    bool is_visible() const;
    void drain_recovery_into(midi::MidiBuffer& midi_in, int frame_count) noexcept;

    std::vector<view::CommandID> commands() const override;
    bool perform_command(view::CommandID id) override;

    view::MusicalTypingKeyboard* keyboard_for_test() {
        return keyboard_.get();
    }
    view::WindowHost* window_for_test() {
        return window_.get();
    }
    bool route_app_key_for_test(const view::KeyEvent& event) {
        return route_app_key(event);
    }

  private:
    struct CallbackState;

    bool emit(const midi::MidiEvent& event);
    void emit_sustain(bool enabled);
    void emit_pitch_bend(float bend);
    void request_note_off_recovery(std::uint8_t note);
    void note_on(int note, float velocity);
    void note_off(int note);
    void release_all_notes();
    bool route_app_key(const view::KeyEvent& event);
    void create_keyboard();

    MidiSink midi_sink_;
    HostFactory host_factory_;
    std::shared_ptr<CallbackState> callback_state_;
    view::CommandRegistry* registry_ = nullptr;
    view::View* routed_root_ = nullptr;
    view::WindowHost* primary_window_ = nullptr;
    std::unique_ptr<view::MusicalTypingKeyboard> keyboard_;
    std::unique_ptr<view::WindowHost> window_;
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

} // namespace pulp::format::detail
