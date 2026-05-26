#include <pulp/midi/midi_ci_pe.hpp>
#include <pulp/midi/mcoded7.hpp>
#include <pulp/runtime/zip.hpp>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace pulp::midi {

// ── Wire helpers ────────────────────────────────────────────────────────

namespace {

void push_muid(std::vector<uint8_t>& buf, MUID muid) {
    buf.push_back(muid.value & 0x7F);
    buf.push_back((muid.value >> 7) & 0x7F);
    buf.push_back((muid.value >> 14) & 0x7F);
    buf.push_back((muid.value >> 21) & 0x7F);
}

void push_u16_le7(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(v & 0x7F);
    buf.push_back((v >> 7) & 0x7F);
}

uint16_t load_u16_le7(const uint8_t* p) {
    return static_cast<uint16_t>(p[0])
         | static_cast<uint16_t>(p[1] << 7);
}

bool is_pe_message(uint8_t sub_id) {
    switch (static_cast<PeMessageType>(sub_id)) {
        case PeMessageType::GetInquiry:
        case PeMessageType::GetReply:
        case PeMessageType::SetInquiry:
        case PeMessageType::SetReply:
        case PeMessageType::SubscribeInquiry:
        case PeMessageType::SubscribeReply:
        case PeMessageType::Notify:
            return true;
    }
    return false;
}

} // namespace

// ── pe_build_message / pe_parse_message ─────────────────────────────────

std::vector<uint8_t> pe_build_message(PeMessageType pe_type,
                                      uint8_t ci_version,
                                      MUID source,
                                      MUID destination,
                                      uint8_t request_id,
                                      std::string_view header_json,
                                      uint16_t total_chunks,
                                      uint16_t chunk_number,
                                      const uint8_t* payload,
                                      std::size_t payload_size) {
    std::vector<uint8_t> msg;
    msg.reserve(32 + header_json.size() * 2 + payload_size * 2);

    // Universal SysEx + CI header.
    msg.push_back(0xF0);
    msg.push_back(0x7E);
    msg.push_back(0x7F);
    msg.push_back(0x0D);
    msg.push_back(static_cast<uint8_t>(pe_type));
    msg.push_back(ci_version);
    push_muid(msg, source);
    push_muid(msg, destination);

    // PE body.
    msg.push_back(request_id & 0x7F);

    auto enc_header = mcoded7_encode(
        reinterpret_cast<const uint8_t*>(header_json.data()),
        header_json.size());
    push_u16_le7(msg, static_cast<uint16_t>(enc_header.size()));
    msg.insert(msg.end(), enc_header.begin(), enc_header.end());

    push_u16_le7(msg, total_chunks);
    push_u16_le7(msg, chunk_number);

    auto enc_payload = (payload_size == 0)
        ? std::vector<uint8_t>{}
        : mcoded7_encode(payload, payload_size);
    push_u16_le7(msg, static_cast<uint16_t>(enc_payload.size()));
    msg.insert(msg.end(), enc_payload.begin(), enc_payload.end());

    msg.push_back(0xF7);
    return msg;
}

std::optional<PeChunk> pe_parse_message(const uint8_t* data, std::size_t size) {
    if (data == nullptr) return std::nullopt;
    // Min: F0 7E 7F 0D sub-id ver src(4) dst(4) req hdr_len(2)
    //      total(2) chunk(2) pay_len(2) F7 = 22 bytes.
    if (size < 22) return std::nullopt;
    if (data[0] != 0xF0 || data[1] != 0x7E || data[3] != 0x0D) return std::nullopt;
    if (!is_pe_message(data[4])) return std::nullopt;
    if (data[size - 1] != 0xF7) return std::nullopt;

    std::size_t p = 14;  // start of PE body
    PeChunk chunk;
    chunk.request_id = data[p++] & 0x7F;

    if (p + 2 > size - 1) return std::nullopt;
    uint16_t hdr_len = load_u16_le7(data + p);
    p += 2;
    if (p + hdr_len > size - 1) return std::nullopt;
    auto hdr_dec = mcoded7_decode(data + p, hdr_len);
    // hdr_dec.empty() with hdr_len > 0 means malformed Mcoded7.
    if (hdr_len > 0 && hdr_dec.empty()) return std::nullopt;
    chunk.header_json.assign(hdr_dec.begin(), hdr_dec.end());
    p += hdr_len;

    if (p + 6 > size - 1) return std::nullopt;
    chunk.total_chunks = load_u16_le7(data + p);
    p += 2;
    chunk.chunk_number = load_u16_le7(data + p);
    p += 2;
    uint16_t pay_len = load_u16_le7(data + p);
    p += 2;
    if (p + pay_len > size - 1) return std::nullopt;
    if (pay_len > 0) {
        chunk.payload = mcoded7_decode(data + p, pay_len);
        if (chunk.payload.empty()) return std::nullopt;
    }
    p += pay_len;

    if (p != size - 1) return std::nullopt;  // junk before F7
    return chunk;
}

std::vector<std::vector<uint8_t>>
pe_split_into_chunks(PeMessageType pe_type,
                     uint8_t ci_version,
                     MUID source,
                     MUID destination,
                     uint8_t request_id,
                     std::string_view header_json,
                     const uint8_t* payload,
                     std::size_t payload_size,
                     std::size_t max_payload_bytes) {
    if (max_payload_bytes == 0) max_payload_bytes = 1;
    std::vector<std::vector<uint8_t>> chunks;

    if (payload_size == 0) {
        chunks.push_back(pe_build_message(
            pe_type, ci_version, source, destination,
            request_id, header_json,
            /*total*/ 1, /*num*/ 1,
            nullptr, 0));
        return chunks;
    }

    std::size_t total = (payload_size + max_payload_bytes - 1) / max_payload_bytes;
    if (total > 0x3FFF) total = 0x3FFF;  // 14-bit ceiling

    for (std::size_t i = 0; i < total; ++i) {
        std::size_t off = i * max_payload_bytes;
        std::size_t len = std::min(max_payload_bytes, payload_size - off);
        chunks.push_back(pe_build_message(
            pe_type, ci_version, source, destination,
            request_id, header_json,
            static_cast<uint16_t>(total),
            static_cast<uint16_t>(i + 1),
            payload + off, len));
    }
    return chunks;
}

// ── PeReassembler ───────────────────────────────────────────────────────

std::optional<PeChunk> PeReassembler::push(PeChunk chunk) {
    if (chunk.total_chunks == 0 || chunk.chunk_number == 0) return std::nullopt;
    if (chunk.chunk_number > chunk.total_chunks) return std::nullopt;

    auto& slot = slots_[chunk.request_id];
    if (slot.total == 0) {
        slot.total = chunk.total_chunks;
        slot.header_json = chunk.header_json;
        slot.seen.assign(chunk.total_chunks, false);
        slot.chunks.assign(chunk.total_chunks, std::vector<uint8_t>{});
    } else if (slot.total != chunk.total_chunks) {
        // Sender changed its mind mid-transfer — abort.
        slots_.erase(chunk.request_id);
        return std::nullopt;
    }

    const std::size_t idx = chunk.chunk_number - 1;
    if (slot.seen[idx]) {
        // Duplicate chunk: ignore but don't error.
        return std::nullopt;
    }
    slot.seen[idx] = true;
    slot.received++;
    slot.chunks[idx] = std::move(chunk.payload);

    if (slot.received != slot.total) return std::nullopt;

    // Completion: stitch chunks in order.
    PeChunk out;
    out.request_id = chunk.request_id;
    out.total_chunks = slot.total;
    out.chunk_number = slot.total;
    out.header_json = slot.header_json;
    for (auto& part : slot.chunks) {
        out.payload.insert(out.payload.end(), part.begin(), part.end());
    }
    slots_.erase(chunk.request_id);
    return out;
}

void PeReassembler::cancel(uint8_t request_id) {
    slots_.erase(request_id);
}

// ── PE JSON header (resource/command/status only) ───────────────────────

namespace {

void json_escape(std::ostringstream& os, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
}

// Tiny PE-header JSON value extractor. Handles only flat string and number
// fields at the top level — which is all the framing layer needs.
// Returns empty optional on missing key or parse failure.
std::optional<std::string> json_get_string(std::string_view json,
                                           std::string_view key) {
    std::string needle = "\"";
    needle += std::string(key);
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += needle.size();
    // skip ws + colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
                                  json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char esc = json[pos + 1];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                default: return std::nullopt;
            }
            pos += 2;
        } else {
            out += json[pos++];
        }
    }
    if (pos >= json.size()) return std::nullopt;
    return out;
}

std::optional<int> json_get_int(std::string_view json, std::string_view key) {
    std::string needle = "\"";
    needle += std::string(key);
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' ||
                                  json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return std::nullopt;
    bool neg = false;
    if (json[pos] == '-') { neg = true; ++pos; }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9')
        return std::nullopt;
    int v = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        v = v * 10 + (json[pos] - '0');
        ++pos;
    }
    return neg ? -v : v;
}

} // namespace

std::string pe_header_make(std::string_view resource,
                           std::string_view command,
                           int status) {
    std::ostringstream os;
    os << "{\"resource\":\"";
    json_escape(os, resource);
    os << "\"";
    if (!command.empty()) {
        os << ",\"command\":\"";
        json_escape(os, command);
        os << "\"";
    }
    os << ",\"status\":" << status << "}";
    return os.str();
}

bool pe_header_parse(std::string_view json,
                     std::string* resource,
                     std::string* command,
                     int* status) {
    if (json.empty()) return false;
    // Cheap structural check — full JSON validation is the application's
    // concern; we only need to pull the framing-relevant keys.
    auto r = json_get_string(json, "resource");
    if (resource) {
        if (r) *resource = *r;
        else resource->clear();
    }
    auto c = json_get_string(json, "command");
    if (command) {
        if (c) *command = *c;
        else command->clear();
    }
    auto s = json_get_int(json, "status");
    if (status) {
        *status = s.value_or(200);
    }
    return r.has_value() || c.has_value() || s.has_value();
}

// ── zlib payload encoding (MIDI-CI 1.2 §6.3) ────────────────────────────

std::optional<std::vector<uint8_t>> pe_compress(const uint8_t* data, std::size_t size) {
    return pulp::runtime::zlib_compress(data, size);
}

std::optional<std::vector<uint8_t>> pe_decompress(const uint8_t* data, std::size_t size) {
    // gzip_decompress() already accepts zlib (RFC 1950) input on the
    // inflate side — see core/runtime/include/pulp/runtime/zip.hpp.
    return pulp::runtime::gzip_decompress(data, size);
}

// ── PeSubscriptionManager ───────────────────────────────────────────────

std::string PeSubscriptionManager::subscribe(std::string_view resource,
                                             MUID subscriber) {
    PeSubscription s;
    s.resource = std::string(resource);
    s.subscriber = subscriber;
    s.subscription_id = "sub-" + std::to_string(next_id_++);
    subs_.push_back(s);
    return s.subscription_id;
}

bool PeSubscriptionManager::unsubscribe(std::string_view subscription_id) {
    auto it = std::find_if(subs_.begin(), subs_.end(),
        [&](const PeSubscription& s) {
            return s.subscription_id == subscription_id;
        });
    if (it == subs_.end()) return false;
    subs_.erase(it);
    return true;
}

std::vector<PeSubscription>
PeSubscriptionManager::subscribers_of(std::string_view resource) const {
    std::vector<PeSubscription> out;
    for (const auto& s : subs_) {
        if (s.resource == resource) out.push_back(s);
    }
    return out;
}

}  // namespace pulp::midi
