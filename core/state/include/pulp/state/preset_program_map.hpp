#pragma once

/// @file preset_program_map.hpp
/// Which preset answers to which MIDI Bank Select + Program Change number.
///
/// A bank is a 14-bit number (Bank Select MSB * 128 + LSB) holding up to 128
/// program slots, each naming a preset by its library key. Assignments are
/// keyed by preset key, so the number a preset answers to survives updates that
/// add presets, and a preset that goes missing leaves its slot empty instead of
/// shifting the rest. Pure data and arithmetic: nothing here touches a file, a
/// thread or a host.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::state {

inline constexpr std::uint16_t kMaxProgramBank = 16383; ///< 14-bit Bank Select
inline constexpr std::uint8_t kMaxProgramNumber = 127;

struct BankProgram {
    std::uint16_t bank = 0;
    std::uint8_t program = 0;

    friend bool operator==(const BankProgram& a, const BankProgram& b) {
        return a.bank == b.bank && a.program == b.program;
    }
    friend bool operator<(const BankProgram& a, const BankProgram& b) {
        return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
    }
};

/// Bank Select value for a controller pair.
constexpr std::uint16_t make_bank(std::uint8_t msb, std::uint8_t lsb) {
    return static_cast<std::uint16_t>(((msb & 0x7F) << 7) | (lsb & 0x7F));
}

/// "bank:program" as shown to the user ("3:12"); a bare "12" means bank 0.
/// Nullopt when the text is not two in-range numbers.
std::optional<BankProgram> parse_bank_program(std::string_view text);
std::string format_bank_program(BankProgram position);

struct ProgramBank {
    std::uint16_t number = 0;
    std::string name;
    /// The preset source this bank was filled from ("factory:", "user:",
    /// "content:<pack>:"); empty for a bank the user made by hand.
    std::string scope;
    std::map<std::uint8_t, std::string> slots; ///< program number -> preset key
};

/// New presets of one source, in the order they should take free slots.
struct ProgramPinGroup {
    std::string scope;
    std::string label; ///< Name for a bank created for this group
    std::vector<std::string> keys;
};

class ProgramMap {
  public:
    const std::vector<ProgramBank>& banks() const {
        return banks_;
    }
    const ProgramBank* find_bank(std::uint16_t number) const;
    bool empty() const {
        return banks_.empty();
    }

    std::optional<std::string> key_at(BankProgram position) const;
    std::optional<BankProgram> position_of(std::string_view key) const;

    /// Put `key` at `position`, taking it out of any slot it held, and
    /// replacing whatever was there. Creates the bank when it does not exist.
    /// False when the position is out of range.
    bool assign(BankProgram position, const std::string& key);
    /// Empty a slot. False when it was already empty.
    bool clear(BankProgram position);
    bool set_bank_name(std::uint16_t number, const std::string& name);
    /// Change a bank's number. False when `from` does not exist, `to` is out of
    /// range or already taken.
    bool renumber_bank(std::uint16_t from, std::uint16_t to);

    /// Give every listed key the map has not seen before the first free slot
    /// in a bank of its source, creating banks as needed (the factory source
    /// prefers bank 0 and the user source bank 1). Existing assignments never
    /// move, which is what keeps a number stable when an update adds or removes
    /// presets, and a key whose slot the user cleared is not given another: a
    /// key is pinned once. Returns true when the map changed.
    bool pin_defaults(const std::vector<ProgramPinGroup>& groups);

    /// A preset moved or was renamed: its slot and its "seen" mark follow it.
    /// False when `old_key` is unknown or `new_key` already has a slot.
    bool rekey(const std::string& old_key, const std::string& new_key);

    std::string to_json() const;
    static std::optional<ProgramMap> from_json(std::string_view json);

    friend bool operator==(const ProgramMap& a, const ProgramMap& b);

  private:
    ProgramBank* find_bank_mutable(std::uint16_t number);
    ProgramBank& bank_for_insert(std::uint16_t number, const std::string& name,
                                 const std::string& scope);
    std::uint16_t lowest_free_bank(std::uint16_t from) const;

    std::vector<ProgramBank> banks_; ///< Sorted by number
    std::set<std::string> seen_;     ///< Every key ever pinned or assigned
};

} // namespace pulp::state
