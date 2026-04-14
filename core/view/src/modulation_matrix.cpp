#include <pulp/view/modulation_matrix.hpp>

#include <algorithm>
#include <cstring>

namespace pulp::view {

std::size_t ModulationMatrix::add(const ModRoute& route) {
    if (auto existing = find(route.source, route.destination)) {
        routes_[*existing] = route;
        return *existing;
    }
    routes_.push_back(route);
    return routes_.size() - 1;
}

void ModulationMatrix::remove(std::size_t index) {
    if (index >= routes_.size()) return;
    routes_.erase(routes_.begin() + static_cast<ptrdiff_t>(index));
}

void ModulationMatrix::remove_by_destination(ModDestinationId destination) {
    routes_.erase(
        std::remove_if(routes_.begin(), routes_.end(),
                       [destination](const ModRoute& r) {
                           return r.destination == destination;
                       }),
        routes_.end());
}

std::optional<std::size_t> ModulationMatrix::find(
    ModSourceId source, ModDestinationId destination) const {
    for (std::size_t i = 0; i < routes_.size(); ++i) {
        if (routes_[i].source == source && routes_[i].destination == destination)
            return i;
    }
    return std::nullopt;
}

// ── Serialization ────────────────────────────────────────────────────────
//
// Layout (little-endian, tightly packed; matches how StateStore blobs the
// rest of plugin state):
//   magic        u32   'PMM1'
//   route_count  u32
//   for each:
//     source        u32
//     destination   u32
//     depth         f32
//     flags         u8   (bit 0 = bipolar)
//     curve         u8
//     reserved      u16
//
// Total per-route: 16 bytes. Header: 8. Fixed-width so version bumps can
// switch on the magic.

static constexpr uint32_t kMagic = 0x504D4D31;  // "PMM1" big-endian read
static constexpr std::size_t kHeaderSize = 8;
static constexpr std::size_t kRouteSize  = 16;

static void write_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
static void write_f32(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32(out, bits);
}
static uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
static float read_f32(const uint8_t* p) {
    uint32_t bits = read_u32(p);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::vector<uint8_t> ModulationMatrix::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + routes_.size() * kRouteSize);
    write_u32(out, kMagic);
    write_u32(out, static_cast<uint32_t>(routes_.size()));
    for (const auto& r : routes_) {
        write_u32(out, r.source);
        write_u32(out, r.destination);
        write_f32(out, r.depth);
        out.push_back(r.bipolar ? 1 : 0);
        out.push_back(static_cast<uint8_t>(r.curve));
        out.push_back(0);
        out.push_back(0);
    }
    return out;
}

bool ModulationMatrix::deserialize(const uint8_t* data, std::size_t size) {
    if (!data) return false;
    if (size < kHeaderSize) return false;
    if (read_u32(data) != kMagic) return false;
    uint32_t count = read_u32(data + 4);
    if (size < kHeaderSize + static_cast<std::size_t>(count) * kRouteSize)
        return false;

    std::vector<ModRoute> parsed;
    parsed.reserve(count);
    const uint8_t* p = data + kHeaderSize;
    for (uint32_t i = 0; i < count; ++i) {
        ModRoute r;
        r.source      = read_u32(p);
        r.destination = read_u32(p + 4);
        r.depth       = read_f32(p + 8);
        r.bipolar     = (p[12] & 1) != 0;
        r.curve       = static_cast<ModCurve>(p[13]);
        p += kRouteSize;
        parsed.push_back(r);
    }
    routes_ = std::move(parsed);
    return true;
}

}  // namespace pulp::view
