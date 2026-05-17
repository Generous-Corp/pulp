#include <algorithm>
#include <pulp/state/store.hpp>
#include <algorithm>
#include <pulp/runtime/assert.hpp>
#include <choc/memory/choc_Endianness.h>

namespace pulp::state {

void StateStore::add_parameter(const ParamInfo& info) {
    auto index = params_.size();
    params_.push_back(info);
    values_.emplace_back(info.range.default_value);
    id_to_index_[info.id] = index;
}

void StateStore::add_group(const ParamGroup& group) {
    groups_.push_back(group);
}

float StateStore::get_value(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return values_[it->second].get();
}

float StateStore::get_modulated(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return values_[it->second].get_modulated();
}

void StateStore::set_mod_offset(ParamID id, float offset) {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) values_[it->second].set_mod_offset(offset);
}

void StateStore::add_mod_offset(ParamID id, float delta) {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) values_[it->second].add_mod_offset(delta);
}

void StateStore::reset_all_mod() {
    for (auto& v : values_) v.reset_mod();
}

void StateStore::set_value(ParamID id, float value) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto& param = params_[it->second];
    float clamped = std::clamp(value, param.range.min, param.range.max);
    values_[it->second].set(clamped);

    // Copy listeners under lock, then invoke outside lock.
    // This prevents blocking the audio thread if a listener does slow work.
    std::vector<ParamChangeCallback> snapshot;
    {
        std::lock_guard lock(listener_mutex_);
        snapshot = listeners_;
    }
    for (auto& cb : snapshot) {
        if (cb) cb(id, clamped);
    }
}

float StateStore::get_normalized(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return values_[it->second].get_normalized(params_[it->second].range);
}

void StateStore::set_normalized(ParamID id, float normalized) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto value = params_[it->second].range.denormalize(normalized);
    set_value(id, value);
}

float StateStore::get_default(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return params_[it->second].range.default_value;
}

void StateStore::reset_to_default(ParamID id) {
    set_value(id, get_default(id));
}

void StateStore::reset_all_to_defaults() {
    for (const auto& p : params_) {
        set_value(p.id, p.range.default_value);
    }
}

const ParamInfo* StateStore::info(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return nullptr;
    return &params_[it->second];
}

void StateStore::begin_gesture(ParamID id) {
    if (on_begin_gesture_) on_begin_gesture_(id);
}

void StateStore::end_gesture(ParamID id) {
    if (on_end_gesture_) on_end_gesture_(id);
}

void StateStore::add_listener(ParamChangeCallback callback) {
    std::lock_guard lock(listener_mutex_);
    listeners_.push_back(std::move(callback));
}

// ── Serialization ──────────────────────────────────────────────────────────
// Binary format:
//   Header: "PULP" (4 bytes) + version (uint32) + param_count (uint32)
//   Per-param: id (uint32) + value (float)
//   Footer: CRC32 (uint32)

static uint32_t crc32_simple(const uint8_t* data, std::size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::vector<uint8_t> StateStore::serialize() const {
    std::vector<uint8_t> out;
    auto count = static_cast<uint32_t>(params_.size());

    // Header
    out.push_back('P'); out.push_back('U'); out.push_back('L'); out.push_back('P');

    auto write_u32 = [&](uint32_t v) {
        uint8_t buf[4];
        choc::memory::writeLittleEndian(buf, v);
        out.insert(out.end(), buf, buf + 4);
    };

    auto write_float = [&](float v) {
        uint8_t buf[4];
        choc::memory::writeLittleEndian(buf, v);
        out.insert(out.end(), buf, buf + 4);
    };

    write_u32(state_version_);
    write_u32(count);

    // Parameters
    for (std::size_t i = 0; i < params_.size(); ++i) {
        write_u32(params_[i].id);
        write_float(values_[i].get());
    }

    // CRC
    auto crc = crc32_simple(out.data(), out.size());
    write_u32(crc);

    return out;
}

bool StateStore::deserialize(std::span<const uint8_t> data) {
    if (data.size() < 16) return false; // Minimum: header(4) + version(4) + count(4) + crc(4)

    // Check magic
    if (data[0] != 'P' || data[1] != 'U' || data[2] != 'L' || data[3] != 'P')
        return false;

    // Verify CRC
    auto payload_size = data.size() - 4;
    auto stored_crc = choc::memory::readLittleEndian<uint32_t>(data.data() + payload_size);
    auto computed_crc = crc32_simple(data.data(), payload_size);
    if (stored_crc != computed_crc) return false;

    // Read header
    uint32_t version = choc::memory::readLittleEndian<uint32_t>(data.data() + 4);
    if (version > state_version_) return false; // reject future versions we can't parse
    uint32_t count = choc::memory::readLittleEndian<uint32_t>(data.data() + 8);

    // Read parameters
    std::size_t offset = 12;
    for (uint32_t i = 0; i < count && offset + 8 <= payload_size; ++i) {
        ParamID id = choc::memory::readLittleEndian<uint32_t>(data.data() + offset);
        float value = choc::memory::readLittleEndian<float>(data.data() + offset + 4);
        offset += 8;

        // Only set if we know this parameter (forward compatibility)
        auto it = id_to_index_.find(id);
        if (it != id_to_index_.end()) {
            const auto index = it->second;
            const auto& range = params_[index].range;
            values_[index].set(std::clamp(value, range.min, range.max));
        }
    }

    return true;
}

} // namespace pulp::state
