#include <pulp/state/preset_program_map.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace pulp::state {

namespace {

constexpr std::uint16_t kFactoryBank = 0;
constexpr std::uint16_t kUserBank = 1;

bool parse_program(const std::string& text, std::uint8_t& out) {
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < 0 ||
        value > kMaxProgramNumber)
        return false;
    out = static_cast<std::uint8_t>(value);
    return true;
}

} // namespace

bool operator==(const ProgramMap& a, const ProgramMap& b) {
    if (a.banks_.size() != b.banks_.size() || a.seen_ != b.seen_)
        return false;
    for (std::size_t i = 0; i < a.banks_.size(); ++i) {
        const auto& x = a.banks_[i];
        const auto& y = b.banks_[i];
        if (x.number != y.number || x.name != y.name || x.scope != y.scope || x.slots != y.slots)
            return false;
    }
    return true;
}

const ProgramBank* ProgramMap::find_bank(std::uint16_t number) const {
    for (const auto& b : banks_)
        if (b.number == number)
            return &b;
    return nullptr;
}

ProgramBank* ProgramMap::find_bank_mutable(std::uint16_t number) {
    for (auto& b : banks_)
        if (b.number == number)
            return &b;
    return nullptr;
}

ProgramBank& ProgramMap::bank_for_insert(std::uint16_t number, const std::string& name,
                                         const std::string& scope) {
    if (auto* existing = find_bank_mutable(number))
        return *existing;
    ProgramBank bank;
    bank.number = number;
    bank.name = name;
    bank.scope = scope;
    auto it = std::lower_bound(banks_.begin(), banks_.end(), number,
                               [](const ProgramBank& b, std::uint16_t n) { return b.number < n; });
    return *banks_.insert(it, std::move(bank));
}

std::uint16_t ProgramMap::lowest_free_bank(std::uint16_t from) const {
    std::uint16_t n = from;
    while (n <= kMaxProgramBank && find_bank(n))
        ++n;
    return n;
}

std::optional<std::string> ProgramMap::key_at(BankProgram position) const {
    const auto* bank = find_bank(position.bank);
    if (!bank)
        return std::nullopt;
    auto it = bank->slots.find(position.program);
    if (it == bank->slots.end())
        return std::nullopt;
    return it->second;
}

std::optional<BankProgram> ProgramMap::position_of(std::string_view key) const {
    for (const auto& bank : banks_)
        for (const auto& [program, slot_key] : bank.slots)
            if (slot_key == key)
                return BankProgram{bank.number, program};
    return std::nullopt;
}

bool ProgramMap::assign(BankProgram position, const std::string& key) {
    if (position.bank > kMaxProgramBank || position.program > kMaxProgramNumber || key.empty())
        return false;
    if (const auto held = position_of(key)) {
        auto* from = find_bank_mutable(held->bank);
        if (from)
            from->slots.erase(held->program);
    }
    bank_for_insert(position.bank, {}, {}).slots[position.program] = key;
    seen_.insert(key);
    return true;
}

bool ProgramMap::clear(BankProgram position) {
    auto* bank = find_bank_mutable(position.bank);
    return bank && bank->slots.erase(position.program) > 0;
}

bool ProgramMap::set_bank_name(std::uint16_t number, const std::string& name) {
    auto* bank = find_bank_mutable(number);
    if (!bank)
        return false;
    bank->name = name;
    return true;
}

bool ProgramMap::renumber_bank(std::uint16_t from, std::uint16_t to) {
    if (to > kMaxProgramBank || find_bank(to) || !find_bank(from))
        return false;
    if (from == to)
        return true;
    auto it = std::find_if(banks_.begin(), banks_.end(),
                           [&](const ProgramBank& b) { return b.number == from; });
    ProgramBank moved = std::move(*it);
    banks_.erase(it);
    moved.number = to;
    auto at = std::lower_bound(banks_.begin(), banks_.end(), to,
                               [](const ProgramBank& b, std::uint16_t n) { return b.number < n; });
    banks_.insert(at, std::move(moved));
    return true;
}

bool ProgramMap::pin_defaults(const std::vector<ProgramPinGroup>& groups) {
    bool changed = false;
    for (const auto& group : groups) {
        std::vector<const std::string*> pending;
        for (const auto& key : group.keys)
            if (!seen_.count(key) && !position_of(key))
                pending.push_back(&key);
        if (pending.empty())
            continue;

        std::size_t next = 0;
        auto fill = [&](ProgramBank& bank) {
            for (int p = 0; p <= kMaxProgramNumber && next < pending.size(); ++p) {
                const auto program = static_cast<std::uint8_t>(p);
                if (bank.slots.count(program))
                    continue;
                seen_.insert(*pending[next]);
                bank.slots[program] = *pending[next++];
                changed = true;
            }
        };

        // Banks this source already owns come first, lowest number first.
        std::vector<std::uint16_t> owned;
        for (const auto& b : banks_)
            if (!group.scope.empty() && b.scope == group.scope)
                owned.push_back(b.number);
        for (auto number : owned) {
            fill(*find_bank_mutable(number));
            if (next >= pending.size())
                break;
        }
        while (next < pending.size()) {
            std::uint16_t number = lowest_free_bank(2);
            if (group.scope == "factory:" && !find_bank(kFactoryBank))
                number = kFactoryBank;
            else if (group.scope == "user:" && !find_bank(kUserBank))
                number = kUserBank;
            if (number > kMaxProgramBank)
                break; // no free bank left
            fill(bank_for_insert(number, group.label, group.scope));
        }
    }
    return changed;
}

bool ProgramMap::rekey(const std::string& old_key, const std::string& new_key) {
    if (old_key == new_key || position_of(new_key))
        return false;
    bool moved = false;
    for (auto& bank : banks_)
        for (auto& [program, key] : bank.slots)
            if (key == old_key) {
                key = new_key;
                moved = true;
            }
    if (seen_.erase(old_key) > 0) {
        seen_.insert(new_key);
        moved = true;
    }
    return moved;
}

std::string ProgramMap::to_json() const {
    std::ostringstream out;
    out << "{\"banks\":[";
    for (std::size_t i = 0; i < banks_.size(); ++i) {
        const auto& b = banks_[i];
        if (i > 0)
            out << ",";
        out << "{\"number\":" << b.number
            << ",\"name\":" << choc::json::getEscapedQuotedString(b.name)
            << ",\"scope\":" << choc::json::getEscapedQuotedString(b.scope) << ",\"slots\":{";
        bool first = true;
        for (const auto& [program, key] : b.slots) {
            if (!first)
                out << ",";
            first = false;
            out << "\"" << static_cast<int>(program)
                << "\":" << choc::json::getEscapedQuotedString(key);
        }
        out << "}}";
    }
    out << "]";
    if (!seen_.empty()) {
        out << ",\"seen\":[";
        bool first = true;
        for (const auto& key : seen_) {
            if (!first)
                out << ",";
            first = false;
            out << choc::json::getEscapedQuotedString(key);
        }
        out << "]";
    }
    out << "}";
    return out.str();
}

std::optional<ProgramMap> ProgramMap::from_json(std::string_view json) {
    ProgramMap map;
    try {
        auto doc = choc::json::parse(json);
        if (!doc.isObject() || !doc.hasObjectMember("banks") || !doc["banks"].isArray())
            return std::nullopt;
        auto banks = doc["banks"];
        for (std::uint32_t i = 0; i < banks.size(); ++i) {
            auto b = banks[i];
            if (!b.isObject() || !b.hasObjectMember("number"))
                return std::nullopt;
            auto n = b["number"];
            const std::int64_t number =
                n.isInt32() ? n.getInt32() : (n.isInt64() ? n.getInt64() : -1);
            if (number < 0 || number > kMaxProgramBank ||
                map.find_bank(static_cast<std::uint16_t>(number)))
                return std::nullopt;
            ProgramBank bank;
            bank.number = static_cast<std::uint16_t>(number);
            if (b.hasObjectMember("name") && b["name"].isString())
                bank.name = std::string(b["name"].getString());
            if (b.hasObjectMember("scope") && b["scope"].isString())
                bank.scope = std::string(b["scope"].getString());
            if (b.hasObjectMember("slots") && b["slots"].isObject()) {
                auto slots = b["slots"];
                for (std::uint32_t k = 0; k < slots.size(); ++k) {
                    auto member = slots.getObjectMemberAt(k);
                    std::uint8_t program = 0;
                    if (!parse_program(std::string(member.name), program) ||
                        !member.value.isString())
                        return std::nullopt;
                    bank.slots[program] = std::string(member.value.getString());
                }
            }
            map.banks_.push_back(std::move(bank));
        }
    } catch (...) {
        return std::nullopt;
    }
    try {
        auto doc = choc::json::parse(json);
        if (doc.hasObjectMember("seen") && doc["seen"].isArray()) {
            auto seen = doc["seen"];
            for (std::uint32_t i = 0; i < seen.size(); ++i)
                if (seen[i].isString())
                    map.seen_.insert(std::string(seen[i].getString()));
        }
    } catch (...) {
        return std::nullopt;
    }
    for (const auto& b : map.banks_)
        for (const auto& [program, key] : b.slots)
            map.seen_.insert(key);
    std::sort(map.banks_.begin(), map.banks_.end(),
              [](const ProgramBank& a, const ProgramBank& b) { return a.number < b.number; });
    // A key may hold only one slot; a hand-edited file that repeats one is refused.
    std::vector<std::string> seen;
    for (const auto& b : map.banks_)
        for (const auto& [program, key] : b.slots)
            seen.push_back(key);
    std::sort(seen.begin(), seen.end());
    if (std::adjacent_find(seen.begin(), seen.end()) != seen.end())
        return std::nullopt;
    return map;
}

} // namespace pulp::state
