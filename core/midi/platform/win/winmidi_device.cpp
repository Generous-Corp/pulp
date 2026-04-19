#include <pulp/midi/device.hpp>
#include <pulp/runtime/log.hpp>

#ifndef _WIN32
#error "winmidi_device.cpp is Windows-only"
#endif

#include <pulp/platform/win32_sane.hpp>
#include <mmeapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::midi::win {

// ── Helpers ──────────────────────────────────────────────────────────────

static std::string wide_to_utf8(const wchar_t* wide) {
    if (!wide || !wide[0]) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');  // len includes null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// ── WinMidiInput ─────────────────────────────────────────────────────────

class WinMidiInput : public MidiInput {
public:
    ~WinMidiInput() override { close(); }

    void set_sysex_callback(MidiSysexCallback cb) override {
        sysex_callback_ = std::move(cb);
    }

    bool open(const std::string& port_id, MidiInputCallback callback) override {
        callback_ = std::move(callback);

        UINT device_id = 0;
        if (!port_id.empty()) {
            device_id = static_cast<UINT>(std::stoul(port_id));
        }

        UINT num_devs = midiInGetNumDevs();
        if (num_devs == 0 || device_id >= num_devs) {
            runtime::log_error("WinMIDI: no input devices available (requested {})", device_id);
            return false;
        }

        // Snapshot QPC frequency + open-time tick so per-event
        // timestamps are seconds-since-open with sub-millisecond
        // resolution. mmeapi's param2 is only millisecond
        // resolution — too coarse for high-rate sources.
        QueryPerformanceFrequency(&qpc_freq_);
        QueryPerformanceCounter(&qpc_open_);

        MMRESULT result = midiInOpen(&handle_, device_id,
            reinterpret_cast<DWORD_PTR>(midi_in_callback),
            reinterpret_cast<DWORD_PTR>(this),
            CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            runtime::log_error("WinMIDI: could not open input device {} (error {})", device_id, result);
            return false;
        }

        // Prepare and queue SysEx receive buffers (#19 / #239).
        // Four 4 KB buffers — enough headroom for typical device
        // dumps without flooding the driver. Driver returns each
        // via MIM_LONGDATA when full or when an F7 arrives; we
        // re-add it after firing the callback.
        for (auto& slot : sysex_slots_) {
            slot.bytes.resize(kSysexBufBytes);
            ZeroMemory(&slot.hdr, sizeof(slot.hdr));
            slot.hdr.lpData         = reinterpret_cast<LPSTR>(slot.bytes.data());
            slot.hdr.dwBufferLength = static_cast<DWORD>(slot.bytes.size());
            slot.hdr.dwUser         = reinterpret_cast<DWORD_PTR>(&slot);
            if (midiInPrepareHeader(handle_, &slot.hdr, sizeof(slot.hdr))
                    == MMSYSERR_NOERROR) {
                midiInAddBuffer(handle_, &slot.hdr, sizeof(slot.hdr));
            }
        }

        result = midiInStart(handle_);
        if (result != MMSYSERR_NOERROR) {
            runtime::log_error("WinMIDI: could not start input (error {})", result);
            for (auto& slot : sysex_slots_) {
                if (slot.hdr.dwFlags & MHDR_PREPARED) {
                    midiInUnprepareHeader(handle_, &slot.hdr, sizeof(slot.hdr));
                }
            }
            midiInClose(handle_);
            handle_ = nullptr;
            return false;
        }

        is_open_ = true;
        return true;
    }

    void close() override {
        if (handle_) {
            // Set the closing flag BEFORE midiInReset so any
            // MIM_LONGDATA callback fired during reset (or already in
            // flight when stop() races with an arriving SysEx) skips
            // the midiInAddBuffer re-queue. Without this, a re-queued
            // header collides with the unprepare/close sequence and
            // either leaks (MIDIERR_STILLPLAYING) or invalidates
            // memory the callback's still walking. See #438 P1
            // Codex review on #388.
            closing_.store(true, std::memory_order_release);
            midiInStop(handle_);
            // midiInReset returns any pending SysEx buffers via
            // MIM_LONGDATA with dwBytesRecorded==0. Unprepare each
            // header before closing the device.
            midiInReset(handle_);
            for (auto& slot : sysex_slots_) {
                if (slot.hdr.dwFlags & MHDR_PREPARED) {
                    midiInUnprepareHeader(handle_, &slot.hdr, sizeof(slot.hdr));
                }
            }
            midiInClose(handle_);
            handle_ = nullptr;
        }
        for (auto& slot : sysex_slots_) {
            slot.bytes.clear();
            ZeroMemory(&slot.hdr, sizeof(slot.hdr));
        }
        is_open_ = false;
        closing_.store(false, std::memory_order_release);
    }

    bool is_open() const override { return is_open_; }

private:
    static constexpr int    kSysexSlots    = 4;
    static constexpr size_t kSysexBufBytes = 4 * 1024;

    struct SysexSlot {
        MIDIHDR              hdr{};
        std::vector<uint8_t> bytes;
    };

    static void CALLBACK midi_in_callback(
        HMIDIIN, UINT msg, DWORD_PTR instance,
        DWORD_PTR param1, DWORD_PTR /*param2*/)
    {
        auto* self = reinterpret_cast<WinMidiInput*>(instance);

        if (msg == MIM_DATA && self->callback_) {
            auto status = static_cast<uint8_t>(param1 & 0xFF);
            auto data1  = static_cast<uint8_t>((param1 >> 8) & 0xFF);
            auto data2  = static_cast<uint8_t>((param1 >> 16) & 0xFF);

            MidiEvent evt;
            evt.message   = choc::midi::ShortMessage(status, data1, data2);
            evt.timestamp = self->qpc_seconds_since_open();
            self->callback_(evt);
            return;
        }

        if (msg == MIM_LONGDATA) {
            auto* hdr = reinterpret_cast<LPMIDIHDR>(param1);
            if (!hdr || hdr->dwBytesRecorded == 0) return;

            if (self->sysex_callback_) {
                std::vector<uint8_t> bytes(
                    reinterpret_cast<const uint8_t*>(hdr->lpData),
                    reinterpret_cast<const uint8_t*>(hdr->lpData)
                        + hdr->dwBytesRecorded);
                self->sysex_callback_(bytes,
                    self->qpc_seconds_since_open());
            }

            // Re-arm the buffer for the next packet — but skip if the
            // device is closing. midiInReset can deliver the last
            // batch of buffers with non-zero dwBytesRecorded; re-queuing
            // them races with the unprepare/close sequence and can
            // leave headers MIDIERR_STILLPLAYING. See #438 P1 / #388.
            if (self->closing_.load(std::memory_order_acquire)) return;
            hdr->dwBytesRecorded = 0;
            midiInAddBuffer(self->handle_, hdr, sizeof(*hdr));
            return;
        }

        if (msg == MIM_LONGERROR) {
            // Long packet was malformed — re-arm the buffer (unless
            // we're closing, same race window as MIM_LONGDATA above).
            if (self->closing_.load(std::memory_order_acquire)) return;
            auto* hdr = reinterpret_cast<LPMIDIHDR>(param1);
            if (hdr) {
                hdr->dwBytesRecorded = 0;
                midiInAddBuffer(self->handle_, hdr, sizeof(*hdr));
            }
        }
    }

    double qpc_seconds_since_open() const {
        if (qpc_freq_.QuadPart == 0) return 0.0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const auto ticks = now.QuadPart - qpc_open_.QuadPart;
        return static_cast<double>(ticks)
             / static_cast<double>(qpc_freq_.QuadPart);
    }

    HMIDIIN            handle_ = nullptr;
    MidiInputCallback  callback_;
    MidiSysexCallback  sysex_callback_;
    bool               is_open_ = false;
    LARGE_INTEGER      qpc_freq_{};
    LARGE_INTEGER      qpc_open_{};
    SysexSlot          sysex_slots_[kSysexSlots]{};
    // closing_ guards the MIM_LONGDATA / MIM_LONGERROR re-queue paths
    // during close() so a callback racing with midiInReset doesn't add
    // a buffer that's about to be unprepared. See #438 P1 / #388.
    std::atomic<bool>  closing_{false};
};

// ── WinMidiOutput ────────────────────────────────────────────────────────

class WinMidiOutput : public MidiOutput {
public:
    ~WinMidiOutput() override { close(); }

    bool open(const std::string& port_id) override {
        UINT device_id = 0;
        if (!port_id.empty()) {
            device_id = static_cast<UINT>(std::stoul(port_id));
        }

        UINT num_devs = midiOutGetNumDevs();
        if (num_devs == 0 || device_id >= num_devs) {
            runtime::log_error("WinMIDI: no output devices available (requested {})", device_id);
            return false;
        }

        MMRESULT result = midiOutOpen(&handle_, device_id,
            0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            runtime::log_error("WinMIDI: could not open output device {} (error {})", device_id, result);
            return false;
        }

        is_open_ = true;
        return true;
    }

    void close() override {
        if (handle_) {
            midiOutReset(handle_);
            midiOutClose(handle_);
            handle_ = nullptr;
        }
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

    void send(const MidiEvent& event) override {
        if (!handle_) return;

        const auto* d = event.data();
        // Pack the MIDI message into a DWORD (little-endian: status | data1<<8 | data2<<16)
        DWORD msg = static_cast<DWORD>(d[0])
                  | (static_cast<DWORD>(d[1]) << 8)
                  | (static_cast<DWORD>(d[2]) << 16);

        midiOutShortMsg(handle_, msg);
    }

private:
    HMIDIOUT handle_ = nullptr;
    bool is_open_ = false;
};

// ── WinMidiSystem ────────────────────────────────────────────────────────

class WinMidiSystem : public MidiSystem {
public:
    std::vector<MidiPortInfo> enumerate_inputs() override {
        std::vector<MidiPortInfo> ports;
        UINT count = midiInGetNumDevs();
        for (UINT i = 0; i < count; ++i) {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                MidiPortInfo info;
                info.id = std::to_string(i);
                info.name = wide_to_utf8(caps.szPname);
                info.is_input = true;
                ports.push_back(std::move(info));
            }
        }
        return ports;
    }

    std::vector<MidiPortInfo> enumerate_outputs() override {
        std::vector<MidiPortInfo> ports;
        UINT count = midiOutGetNumDevs();
        for (UINT i = 0; i < count; ++i) {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                MidiPortInfo info;
                info.id = std::to_string(i);
                info.name = wide_to_utf8(caps.szPname);
                info.is_output = true;
                ports.push_back(std::move(info));
            }
        }
        return ports;
    }

    std::unique_ptr<MidiInput> create_input() override {
        return std::make_unique<WinMidiInput>();
    }

    std::unique_ptr<MidiOutput> create_output() override {
        return std::make_unique<WinMidiOutput>();
    }
};

} // namespace pulp::midi::win

// Factory function
namespace pulp::midi {

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<win::WinMidiSystem>();
}

} // namespace pulp::midi
