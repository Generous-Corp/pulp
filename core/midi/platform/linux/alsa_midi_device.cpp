#include <pulp/midi/device.hpp>
#include <pulp/runtime/log.hpp>

#ifndef __linux__
#error "alsa_midi_device.cpp is Linux-only"
#endif

#include <alsa/asoundlib.h>

#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace pulp::midi::linux_platform {

// ── AlsaMidiInput ────────────────────────────────────────────────────────

class AlsaMidiInput : public MidiInput {
public:
    ~AlsaMidiInput() override { close(); }

    bool open(const std::string& port_id, MidiInputCallback callback) override {
        callback_ = std::move(callback);

        int err = snd_rawmidi_open(&handle_, nullptr,
            port_id.empty() ? "virtual" : port_id.c_str(), SND_RAWMIDI_NONBLOCK);
        if (err < 0) {
            runtime::log_error("ALSA MIDI: could not open input '{}': {}",
                port_id, snd_strerror(err));
            return false;
        }

        is_open_ = true;
        running_.store(true, std::memory_order_release);
        read_thread_ = std::thread([this] { read_thread_func(); });

        return true;
    }

    void close() override {
        running_.store(false, std::memory_order_release);
        if (read_thread_.joinable()) {
            read_thread_.join();
        }
        if (handle_) {
            snd_rawmidi_close(handle_);
            handle_ = nullptr;
        }
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

private:
    void read_thread_func() {
        uint8_t buf[256];

        while (running_.load(std::memory_order_relaxed)) {
            ssize_t n = snd_rawmidi_read(handle_, buf, sizeof(buf));
            if (n < 0) {
                if (n == -EAGAIN) {
                    // No data available — brief sleep to avoid busy-wait
                    struct timespec ts = {0, 1000000}; // 1ms
                    nanosleep(&ts, nullptr);
                    continue;
                }
                break; // Error
            }

            // Parse raw MIDI bytes into events
            for (ssize_t i = 0; i < n; ) {
                uint8_t status = buf[i];
                if (status < 0x80) { ++i; continue; } // Skip data bytes without status

                int msg_len = 3; // Default for most channel messages
                if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                    msg_len = 2; // Program Change, Channel Pressure
                }

                if (i + msg_len <= n) {
                    MidiEvent evt;
                    evt.message = choc::midi::ShortMessage(
                        buf[i],
                        msg_len > 1 ? buf[i + 1] : 0,
                        msg_len > 2 ? buf[i + 2] : 0);
                    evt.timestamp = 0.0; // Raw MIDI has no timestamps
                    if (callback_) callback_(evt);
                }
                i += msg_len;
            }
        }
    }

    snd_rawmidi_t* handle_ = nullptr;
    MidiInputCallback callback_;
    bool is_open_ = false;
    std::atomic<bool> running_{false};
    std::thread read_thread_;
};

// ── AlsaMidiOutput ───────────────────────────────────────────────────────

class AlsaMidiOutput : public MidiOutput {
public:
    ~AlsaMidiOutput() override { close(); }

    bool open(const std::string& port_id) override {
        int err = snd_rawmidi_open(nullptr, &handle_,
            port_id.empty() ? "virtual" : port_id.c_str(), 0);
        if (err < 0) {
            runtime::log_error("ALSA MIDI: could not open output '{}': {}",
                port_id, snd_strerror(err));
            return false;
        }
        is_open_ = true;
        return true;
    }

    void close() override {
        if (handle_) {
            snd_rawmidi_close(handle_);
            handle_ = nullptr;
        }
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

    void send(const MidiEvent& event) override {
        if (!handle_) return;
        const auto* d = event.data();
        uint8_t buf[3] = {d[0], d[1], d[2]};
        int len = 3;
        if ((d[0] & 0xF0) == 0xC0 || (d[0] & 0xF0) == 0xD0) len = 2;
        snd_rawmidi_write(handle_, buf, len);
    }

private:
    snd_rawmidi_t* handle_ = nullptr;
    bool is_open_ = false;
};

// ── AlsaMidiSystem ───────────────────────────────────────────────────────

class AlsaMidiSystem : public MidiSystem {
public:
    std::vector<MidiPortInfo> enumerate_inputs() override {
        return enumerate_ports(true);
    }

    std::vector<MidiPortInfo> enumerate_outputs() override {
        return enumerate_ports(false);
    }

    std::unique_ptr<MidiInput> create_input() override {
        return std::make_unique<AlsaMidiInput>();
    }

    std::unique_ptr<MidiOutput> create_output() override {
        return std::make_unique<AlsaMidiOutput>();
    }

private:
    std::vector<MidiPortInfo> enumerate_ports(bool inputs) {
        std::vector<MidiPortInfo> ports;

        int card = -1;
        while (snd_card_next(&card) == 0 && card >= 0) {
            char name[64];
            snprintf(name, sizeof(name), "hw:%d", card);

            snd_ctl_t* ctl = nullptr;
            if (snd_ctl_open(&ctl, name, 0) < 0) continue;

            int device = -1;
            while (snd_ctl_rawmidi_next_device(ctl, &device) == 0 && device >= 0) {
                snd_rawmidi_info_t* info = nullptr;
                snd_rawmidi_info_alloca(&info);
                snd_rawmidi_info_set_device(info, device);
                snd_rawmidi_info_set_stream(info,
                    inputs ? SND_RAWMIDI_STREAM_INPUT : SND_RAWMIDI_STREAM_OUTPUT);

                int sub = 0;
                snd_rawmidi_info_set_subdevice(info, sub);
                if (snd_ctl_rawmidi_info(ctl, info) == 0) {
                    MidiPortInfo port;
                    port.id = "hw:" + std::to_string(card) + "," + std::to_string(device);
                    const char* port_name = snd_rawmidi_info_get_name(info);
                    port.name = port_name ? port_name : port.id;
                    port.is_input = inputs;
                    port.is_output = !inputs;
                    ports.push_back(std::move(port));
                }
            }

            snd_ctl_close(ctl);
        }

        return ports;
    }
};

} // namespace pulp::midi::linux_platform

// Factory function
namespace pulp::midi {

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<linux_platform::AlsaMidiSystem>();
}

} // namespace pulp::midi
