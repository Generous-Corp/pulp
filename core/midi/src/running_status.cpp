#include <pulp/midi/running_status.hpp>

namespace pulp::midi {

int RunningStatusParser::expected_data_bytes(uint8_t status) {
    const uint8_t hi = status & 0xF0;
    switch (hi) {
        case 0x80: case 0x90: case 0xA0:
        case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: break;
    }
    // System common: status-specific
    switch (status) {
        case 0xF1: return 1;  // MTC quarter-frame
        case 0xF2: return 2;  // Song position pointer
        case 0xF3: return 1;  // Song select
        case 0xF6: return 0;  // Tune request
        case 0xF8: case 0xFA: case 0xFB: case 0xFC:
        case 0xFE: case 0xFF: return 0;  // Real-time single-byte messages
        default:  return -1; // sysex / unknown
    }
}

void RunningStatusParser::emit_short(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!short_sink_) return;
    MidiEvent ev;
    ev.message = choc::midi::ShortMessage(status, d1, d2);
    short_sink_(ev);
}

void RunningStatusParser::reset() {
    running_status_ = 0;
    current_system_common_ = 0;   // #202 P2: reset must drop this too
    data_count_ = 0;
    data_expected_ = 0;
    in_sysex_ = false;
    sysex_buf_.clear();
}

void RunningStatusParser::feed(const uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        uint8_t b = data[i];

        // Real-time messages (0xF8..0xFF) can interleave anywhere, even
        // inside a sysex — emit immediately and don't disturb running
        // status.
        if (b >= 0xF8) {
            emit_short(b, 0, 0);
            continue;
        }

        if (in_sysex_) {
            if (b == 0xF7) {
                if (sysex_sink_ && !sysex_buf_.empty()) {
                    sysex_sink_(sysex_buf_.data(), sysex_buf_.size());
                }
                sysex_buf_.clear();
                in_sysex_ = false;
            } else if (b < 0x80) {
                sysex_buf_.push_back(b);
            } else {
                // Unexpected non-realtime status byte during sysex —
                // terminate the sysex without emitting and fall through
                // to process this byte fresh.
                sysex_buf_.clear();
                in_sysex_ = false;
                --i;  // reprocess current byte
            }
            continue;
        }

        if (b >= 0x80) {
            if (b == 0xF0) {
                in_sysex_ = true;
                sysex_buf_.clear();
                // System common / sysex resets running status AND any
                // in-flight data-byte accumulator — otherwise bytes after
                // the terminating F7 would complete the pre-sysex message.
                running_status_ = 0;
                data_expected_ = 0;
                data_count_ = 0;
                current_system_common_ = 0;
                continue;
            }
            // Channel voice status or system common
            int expected = expected_data_bytes(b);
            if (expected < 0) continue;  // unknown — drop
            if ((b & 0xF0) >= 0x80 && (b & 0xF0) <= 0xE0) {
                // Channel voice — becomes running status
                running_status_ = b;
                data_expected_ = expected;
                data_count_ = 0;
                if (expected == 0) {
                    emit_short(b, 0, 0);
                }
            } else {
                // System common — cancels running status, handled inline
                running_status_ = 0;
                data_expected_ = expected;
                data_count_ = 0;
                if (expected == 0) {
                    // One-byte system common (e.g. F6 Tune Request) — emit
                    // immediately and do NOT retain it as current_system_
                    // common. Otherwise a stray trailing data byte would
                    // pass the guard below and trigger a phantom extra
                    // emission under the stale status. Fix per #202 P1.
                    emit_short(b, 0, 0);
                    current_system_common_ = 0;
                } else {
                    // F1/F2/F3: bind as one-shot system-common so the
                    // next expected data bytes complete the message.
                    current_system_common_ = b;
                }
            }
        } else {
            // Data byte
            if (data_expected_ == 0 && running_status_ == 0
                && current_system_common_ == 0) {
                // No status to bind to — drop.
                continue;
            }
            data_[data_count_++] = b;
            if (data_count_ >= data_expected_) {
                uint8_t status = running_status_
                               ? running_status_
                               : current_system_common_;
                emit_short(status,
                           data_expected_ > 0 ? data_[0] : 0,
                           data_expected_ > 1 ? data_[1] : 0);
                data_count_ = 0;
                // System-common is one-shot; channel voice sticks.
                if (!running_status_) current_system_common_ = 0;
            }
        }
    }
}

}  // namespace pulp::midi
