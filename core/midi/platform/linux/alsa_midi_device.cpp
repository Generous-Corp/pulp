#include <pulp/midi/ble_midi_registry.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/midi/monotonic_timestamp.hpp>
#include <pulp/midi/raw_midi_parser.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/udev_monitor.hpp>

#ifndef __linux__
#error "alsa_midi_device.cpp is Linux-only"
#endif

#include <alsa/asoundlib.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace pulp::midi::linux_platform {

// ── AlsaMidiInput ────────────────────────────────────────────────────────

class AlsaMidiInput : public MidiInput {
public:
    ~AlsaMidiInput() override { close(); }

    void set_sysex_callback(MidiSysexCallback cb) override {
        sysex_callback_ = std::move(cb);
    }

    bool open(const std::string& port_id, MidiInputCallback callback) override {
        callback_ = std::move(callback);

        // BLE-MIDI ports are not raw-ALSA devices: a connected BLE peripheral is
        // published into the process-wide BleMidiPortRegistry by the BlueZ
        // central, and its MIDI bytes are delivered by the central's GATT-notify
        // decoder. Route the open() to the registry instead of snd_rawmidi_open.
        if (BleMidiPortRegistry::instance().is_input(port_id)) {
            ble_port_id_ = port_id;
            const bool attached = BleMidiPortRegistry::instance().attach_input(
                port_id, callback_, sysex_callback_);
            is_open_ = attached;
            return attached;
        }

        int err = snd_rawmidi_open(&handle_, nullptr,
            port_id.empty() ? "virtual" : port_id.c_str(), SND_RAWMIDI_NONBLOCK);
        if (err < 0) {
            runtime::log_error("ALSA MIDI: could not open input '{}': {}",
                port_id, snd_strerror(err));
            return false;
        }

        is_open_ = true;
        // Base the per-event monotonic timestamps at open time so they read
        // as seconds-since-open, matching the Windows/CoreMIDI backends.
        clock_.reset();
        running_.store(true, std::memory_order_release);
        read_thread_ = std::thread([this] { read_thread_func(); });

        return true;
    }

    void close() override {
        if (!ble_port_id_.empty()) {
            BleMidiPortRegistry::instance().detach_input(ble_port_id_);
            ble_port_id_.clear();
            is_open_ = false;
            return;
        }
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

            // Parse raw MIDI bytes. Shared helper keeps the accumulator
            // logic testable across ALSA and any future raw-byte MIDI
            // transport (see raw_midi_parser.hpp).
            parse_raw_midi_bytes(
                reinterpret_cast<const uint8_t*>(buf),
                static_cast<std::size_t>(n),
                parser_state_,
                [this](uint8_t status, uint8_t d1, uint8_t d2) {
                    if (!callback_) return;
                    MidiEvent evt;
                    evt.message = choc::midi::ShortMessage(status, d1, d2);
                    evt.timestamp = clock_.seconds_since_open();
                    callback_(evt);
                },
                [this](const std::vector<uint8_t>& sysex) {
                    if (sysex_callback_)
                        sysex_callback_(sysex, clock_.seconds_since_open());
                });
        }
    }

    snd_rawmidi_t* handle_ = nullptr;
    MidiInputCallback callback_;
    MidiSysexCallback sysex_callback_;
    RawMidiParserState parser_state_;
    MonotonicMidiClock clock_;
    bool is_open_ = false;
    std::atomic<bool> running_{false};
    std::thread read_thread_;
    std::string ble_port_id_;  // non-empty when this input is a BLE-MIDI port
};

// ── AlsaMidiOutput ───────────────────────────────────────────────────────

class AlsaMidiOutput : public MidiOutput {
public:
    ~AlsaMidiOutput() override { close(); }

    bool open(const std::string& port_id) override {
        // BLE-MIDI output ports route through the registry's GATT-write sink
        // published by the BlueZ central, not snd_rawmidi.
        if (BleMidiPortRegistry::instance().is_output(port_id)) {
            ble_sink_ = BleMidiPortRegistry::instance().output_sink(port_id);
            is_open_ = static_cast<bool>(ble_sink_);
            return is_open_;
        }
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
        if (ble_sink_) {
            ble_sink_ = nullptr;
            is_open_ = false;
            return;
        }
        if (handle_) {
            snd_rawmidi_close(handle_);
            handle_ = nullptr;
        }
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

    void send(const MidiEvent& event) override {
        const auto* d = event.data();
        int len = 3;
        if ((d[0] & 0xF0) == 0xC0 || (d[0] & 0xF0) == 0xD0) len = 2;
        if (ble_sink_) {
            ble_sink_(std::vector<uint8_t>(d, d + len));
            return;
        }
        if (!handle_) return;
        uint8_t buf[3] = {d[0], d[1], d[2]};
        snd_rawmidi_write(handle_, buf, len);
    }

private:
    snd_rawmidi_t* handle_ = nullptr;
    bool is_open_ = false;
    std::function<void(const std::vector<uint8_t>&)> ble_sink_;  // BLE output
};

// ── AlsaMidiSystem ───────────────────────────────────────────────────────

class AlsaMidiSystem : public MidiSystem {
public:
    std::vector<MidiPortInfo> enumerate_inputs() override {
        std::vector<MidiPortInfo> ports = enumerate_ports(true);
        // Merge connected BLE-MIDI peripherals — off Apple there is no OS bridge
        // that auto-exposes a GATT stream as an ALSA port, so the BlueZ central
        // publishes them into the process-wide registry and we surface them here.
        auto ble = BleMidiPortRegistry::instance().list_inputs();
        ports.insert(ports.end(), ble.begin(), ble.end());
        return ports;
    }

    std::vector<MidiPortInfo> enumerate_outputs() override {
        std::vector<MidiPortInfo> ports = enumerate_ports(false);
        auto ble = BleMidiPortRegistry::instance().list_outputs();
        ports.insert(ports.end(), ble.begin(), ble.end());
        return ports;
    }

    std::unique_ptr<MidiInput> create_input() override {
        return std::make_unique<AlsaMidiInput>();
    }

    std::unique_ptr<MidiOutput> create_output() override {
        return std::make_unique<AlsaMidiOutput>();
    }

    ~AlsaMidiSystem() override {
        // Stop the monitor thread (which captures `this`) before members.
        hotplug_monitor_.stop();
    }

    /// Start (or stop) a libudev "sound"-subsystem monitor that fires the
    /// stored port-change callback on MIDI card add/remove. The "sound"
    /// subsystem covers ALSA raw-midi devices. Honest no-op for hotplug if
    /// libudev is unavailable (the callback is stored but never fired).
    void set_port_change_callback(PortChangeCallback cb) override {
        const bool want_monitor = static_cast<bool>(cb);
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            port_change_cb_ = std::move(cb);
        }
        if (want_monitor) {
            if (!hotplug_monitor_.running())
                hotplug_monitor_.start({"sound"},
                    [this](runtime::UdevChange) { fire_port_change(); });
        } else {
            hotplug_monitor_.stop();
        }
    }

private:
    void fire_port_change() {
        PortChangeCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            cb = port_change_cb_;
        }
        if (cb) cb();
    }

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

    std::mutex cb_mutex_;
    PortChangeCallback port_change_cb_;
    runtime::UdevMonitor hotplug_monitor_;
};

} // namespace pulp::midi::linux_platform

// Factory function
namespace pulp::midi {

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<linux_platform::AlsaMidiSystem>();
}

} // namespace pulp::midi
