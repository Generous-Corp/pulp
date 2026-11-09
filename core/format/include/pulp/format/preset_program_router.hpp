#pragma once

/// @file preset_program_router.hpp
/// Turns in-band MIDI Bank Select + Program Change into a request to load a
/// preset, without doing the load on the audio thread.
///
/// The audio thread calls `process()` with a block's input MIDI. It tracks the
/// latest Bank Select (CC 0 / CC 32) per channel and, on a Program Change,
/// stores one small request in a single atomic slot: no allocation, no lock, no
/// file access. A later Program Change replaces an earlier one that nobody has
/// taken yet, so a burst loads only its last preset. The message thread polls
/// `take_request()`, resolves the position through the library's program map
/// and loads the preset.
///
/// The router only observes: the MIDI buffer is not modified, so a processor
/// that handles Program Change itself still sees it.

#include <pulp/midi/buffer.hpp>
#include <pulp/state/midi_source_reservation.hpp>
#include <pulp/state/preset_program_map.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pulp::format {

enum class BankSelectMode : std::uint8_t {
    msb_lsb,     ///< 14-bit bank: CC 0 is the high 7 bits, CC 32 the low 7
    msb_only,    ///< The bank is CC 0 alone (0-127); CC 32 is ignored
    ignore_bank, ///< Program Change always addresses bank 0
};

struct ProgramRouterSettings {
    bool enabled = false;
    /// 0 listens on every channel; 1-16 listens on that channel only.
    std::uint8_t channel = 0;
    BankSelectMode mode = BankSelectMode::msb_lsb;
};

class PresetProgramRouter final : public state::MidiSourceReservation {
  public:
    struct Request {
        state::BankProgram position;
        std::uint32_t sequence = 0; ///< Counts every Program Change seen
    };

    /// Any thread. Takes effect on the next message the audio thread sees.
    void set_settings(const ProgramRouterSettings& settings) noexcept {
        settings_.store(pack(settings), std::memory_order_relaxed);
    }
    ProgramRouterSettings settings() const noexcept {
        return unpack(settings_.load(std::memory_order_relaxed));
    }

    /// True when the router is listening to this controller number, so a
    /// feature that maps controllers (MIDI Learn) can leave it alone.
    bool claims_controller(std::uint8_t controller) const noexcept {
        const auto s = settings();
        if (!s.enabled)
            return false;
        switch (s.mode) {
        case BankSelectMode::msb_lsb:
            return controller == 0 || controller == 32;
        case BankSelectMode::msb_only:
            return controller == 0;
        case BankSelectMode::ignore_bank:
            return false;
        }
        return false;
    }

    /// MidiSourceReservation: the sources this router consumes while enabled.
    /// Bank Select (CC 0 and CC 32, and the 14-bit pair on CC 0 whose low half
    /// is CC 32) in the modes that read it, and every Program Change. A source
    /// on an omni channel overlaps whichever channel the router listens to.
    bool claims(const state::MidiSource& source) const noexcept override {
        const auto s = settings();
        if (!s.enabled) return false;
        if (s.channel != 0 && source.channel != state::MidiSource::kOmni &&
            source.channel != s.channel - 1)
            return false;
        using Kind = state::MidiSourceKind;
        switch (source.kind) {
        case Kind::program_change: return true;
        case Kind::cc7:
            return (source.number == 0 && s.mode != BankSelectMode::ignore_bank) ||
                   (source.number == 32 && s.mode == BankSelectMode::msb_lsb);
        case Kind::cc14: return source.number == 0 && s.mode == BankSelectMode::msb_lsb;
        default: return false;
        }
    }
    const char* reason() const noexcept override {
        return "Bank Select and Program Change choose presets";
    }

    // ── Audio thread ────────────────────────────────────────────────────

    /// Scan a block's input MIDI. Real-time safe.
    void process(const midi::MidiBuffer& input) noexcept {
        for (const auto& event : input)
            on_message(event.data(), event.size());
    }

    /// Handle one channel message (status byte first). Real-time safe.
    void on_message(const std::uint8_t* bytes, std::size_t size) noexcept {
        if (size < 2)
            return;
        const std::uint8_t status = bytes[0];
        const std::uint8_t kind = status & 0xF0;
        if (kind != 0xB0 && kind != 0xC0)
            return;
        const auto s = settings();
        if (!s.enabled)
            return;
        const std::uint8_t channel = status & 0x0F;
        if (s.channel != 0 && channel != s.channel - 1)
            return;

        if (kind == 0xB0) {
            if (size < 3)
                return;
            if (bytes[1] == 0)
                msb_[channel] = bytes[2] & 0x7F;
            else if (bytes[1] == 32)
                lsb_[channel] = bytes[2] & 0x7F;
            return;
        }

        std::uint16_t bank = 0;
        switch (s.mode) {
        case BankSelectMode::msb_lsb:
            bank = state::make_bank(msb_[channel], lsb_[channel]);
            break;
        case BankSelectMode::msb_only:
            bank = msb_[channel];
            break;
        case BankSelectMode::ignore_bank:
            bank = 0;
            break;
        }
        const std::uint8_t program = bytes[1] & 0x7F;
        ++sequence_;
        request_.store(pack_request(bank, program, sequence_), std::memory_order_release);
    }

    /// Forget the latched Bank Select values (a new session or transport
    /// reset). Reset All Controllers (CC 121) deliberately does not: General
    /// MIDI leaves Bank Select and program alone.
    void reset() noexcept {
        msb_.fill(0);
        lsb_.fill(0);
    }

    // ── Message thread ──────────────────────────────────────────────────

    /// True when a Program Change is waiting. Real-time safe; does not take it.
    bool has_pending() const noexcept {
        return request_.load(std::memory_order_relaxed) != 0;
    }

    /// The newest unhandled Program Change, if any. Taking it empties the slot.
    std::optional<Request> take_request() noexcept {
        const std::uint64_t v = request_.exchange(0, std::memory_order_acquire);
        if ((v & kValid) == 0)
            return std::nullopt;
        Request r;
        r.position.program = static_cast<std::uint8_t>(v & 0x7F);
        r.position.bank = static_cast<std::uint16_t>((v >> 7) & 0x3FFF);
        r.sequence = static_cast<std::uint32_t>((v >> 21) & 0xFFFFFFFFu);
        return r;
    }

  private:
    static constexpr std::uint64_t kValid = 1ull << 63;

    static std::uint64_t pack_request(std::uint16_t bank, std::uint8_t program,
                                      std::uint32_t sequence) noexcept {
        return kValid | (static_cast<std::uint64_t>(sequence) << 21) |
               (static_cast<std::uint64_t>(bank & 0x3FFF) << 7) | (program & 0x7F);
    }
    static std::uint32_t pack(const ProgramRouterSettings& s) noexcept {
        return (s.enabled ? 1u : 0u) | (static_cast<std::uint32_t>(s.channel & 0x1F) << 1) |
               (static_cast<std::uint32_t>(s.mode) << 6);
    }
    static ProgramRouterSettings unpack(std::uint32_t v) noexcept {
        ProgramRouterSettings s;
        s.enabled = (v & 1u) != 0;
        s.channel = static_cast<std::uint8_t>((v >> 1) & 0x1F);
        s.mode = static_cast<BankSelectMode>((v >> 6) & 0x3);
        return s;
    }

    std::atomic<std::uint32_t> settings_{0};
    std::atomic<std::uint64_t> request_{0};
    std::uint32_t sequence_ = 0;         // audio thread only
    std::array<std::uint8_t, 16> msb_{}; // audio thread only
    std::array<std::uint8_t, 16> lsb_{}; // audio thread only
};

} // namespace pulp::format
