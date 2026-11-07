#pragma once

/// @file mock_midi_system.hpp
/// Scriptable in-memory MIDI backend for tests and headless hosts.
///
/// Ports are added and removed from code, the registered port-change callback
/// fires on every change, and events can be injected into any open input. An
/// input opened on a port that is removed stays "open" the way an OS handle
/// does until the owner closes it, which is what makes stale-handle bugs
/// reproducible.

#include <pulp/midi/device.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::midi {

class MockMidiSystem : public MidiSystem {
public:
    /// Add (or replace) a port and notify the port-change callback.
    void add_port(const std::string& id, const std::string& name, bool is_input = true) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            remove_locked(id, is_input);
            ports_.push_back({id, name, is_input, !is_input});
        }
        notify();
    }

    /// Remove a port and notify the port-change callback.
    void remove_port(const std::string& id, bool is_input = true) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            remove_locked(id, is_input);
        }
        notify();
    }

    /// Change the port list without notifying, to model a notification that
    /// never arrived. Call `notify()` or refresh the consumer afterwards.
    void set_ports_silently(std::vector<MidiPortInfo> ports) {
        std::lock_guard<std::mutex> lk(mu_);
        ports_ = std::move(ports);
    }

    void notify() {
        PortChangeCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = port_change_cb_;
        }
        if (cb) cb();
    }

    /// Deliver `event` to every open input on `id`. Returns how many received it.
    int inject(const std::string& id, const MidiEvent& event) {
        std::vector<std::shared_ptr<InputState>> targets;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& s : inputs_) {
                if (s->open && s->port_id == id) targets.push_back(s);
            }
        }
        int delivered = 0;
        for (auto& s : targets) {
            if (s->callback) { s->callback(event); ++delivered; }
        }
        return delivered;
    }

    /// Number of currently open inputs on `id`.
    int open_input_count(const std::string& id) const {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& s : inputs_) if (s->open && s->port_id == id) ++n;
        return n;
    }

    /// Total successful input opens since construction.
    int total_opens() const {
        std::lock_guard<std::mutex> lk(mu_);
        return total_opens_;
    }

    /// Make the next `count` open attempts fail, to model a busy device.
    void fail_next_opens(int count) {
        std::lock_guard<std::mutex> lk(mu_);
        failing_opens_ = count;
    }

    std::vector<MidiPortInfo> enumerate_inputs() override { return list(true); }
    std::vector<MidiPortInfo> enumerate_outputs() override { return list(false); }

    std::unique_ptr<MidiInput> create_input() override {
        return std::make_unique<Input>(this);
    }

    std::unique_ptr<MidiOutput> create_output() override {
        return std::make_unique<Output>();
    }

    void set_port_change_callback(PortChangeCallback cb) override {
        std::lock_guard<std::mutex> lk(mu_);
        port_change_cb_ = std::move(cb);
    }

private:
    struct InputState {
        std::string port_id;
        MidiInputCallback callback;
        bool open = false;
    };

    class Input : public MidiInput {
    public:
        explicit Input(MockMidiSystem* owner) : owner_(owner), state_(std::make_shared<InputState>()) {}
        ~Input() override { close(); }

        bool open(const std::string& port_id, MidiInputCallback callback) override {
            std::lock_guard<std::mutex> lk(owner_->mu_);
            if (owner_->failing_opens_ > 0) { --owner_->failing_opens_; return false; }
            const bool exists = std::any_of(owner_->ports_.begin(), owner_->ports_.end(),
                [&](const MidiPortInfo& p) { return p.is_input && p.id == port_id; });
            if (!exists) return false;
            state_->port_id = port_id;
            state_->callback = std::move(callback);
            state_->open = true;
            owner_->inputs_.push_back(state_);
            ++owner_->total_opens_;
            return true;
        }

        void close() override {
            std::lock_guard<std::mutex> lk(owner_->mu_);
            if (!state_->open) return;
            state_->open = false;
            auto& v = owner_->inputs_;
            v.erase(std::remove(v.begin(), v.end(), state_), v.end());
        }

        bool is_open() const override {
            std::lock_guard<std::mutex> lk(owner_->mu_);
            return state_->open;
        }

    private:
        MockMidiSystem* owner_;
        std::shared_ptr<InputState> state_;
    };

    class Output : public MidiOutput {
    public:
        bool open(const std::string&) override { open_ = true; return true; }
        void close() override { open_ = false; }
        bool is_open() const override { return open_; }
        void send(const MidiEvent&) override {}
    private:
        bool open_ = false;
    };

    std::vector<MidiPortInfo> list(bool inputs) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<MidiPortInfo> out;
        for (auto& p : ports_) if (p.is_input == inputs) out.push_back(p);
        return out;
    }

    void remove_locked(const std::string& id, bool is_input) {
        ports_.erase(std::remove_if(ports_.begin(), ports_.end(),
            [&](const MidiPortInfo& p) { return p.id == id && p.is_input == is_input; }),
            ports_.end());
    }

    mutable std::mutex mu_;
    std::vector<MidiPortInfo> ports_;
    std::vector<std::shared_ptr<InputState>> inputs_;
    PortChangeCallback port_change_cb_;
    int total_opens_ = 0;
    int failing_opens_ = 0;
};

} // namespace pulp::midi
