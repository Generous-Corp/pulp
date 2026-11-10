#pragma once
/// @file output_device_session.hpp
/// OutputDeviceSession: keeps one output device open on the device the user
/// chose, and keeps it correct as devices come and go.
///
/// A choice is either "follow the system default output" or a named device id.
/// reconcile() compares the choice with the live device list and reopens only
/// when it must:
///   - following: the default output id changed;
///   - named: the device vanished (the session closes it and reports
///     `missing`) or came back (the session opens a new handle);
///   - either: the open handle is no longer open or running, which is how a
///     replug that never showed a gap in the list is noticed.
/// A named choice never moves because the default moved.
///
/// The identity compared is the device id, not its name. A reopen stops the old
/// callback completely (AudioDevice::stop() returns only after any in-flight
/// block has finished), closes and destroys the old handle, and only then
/// creates the new one, so exactly one callback is live at a time and no call
/// reaches a handle after it was replaced.
///
/// Threading: open(), close() and reconcile() belong to one control thread
/// (normally the main thread). A device-change notification may arrive on an OS
/// thread; forward it to the control thread and call reconcile() there.

#include <pulp/audio/device.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace pulp::audio {

struct OutputChoice {
    bool follow_default = true;
    std::string device_id;  ///< Used when `follow_default` is false.
};

class OutputDeviceSession {
  public:
    enum class State {
        closed,   ///< No choice made, or close() was called.
        running,  ///< The chosen device is open with the callback running.
        missing   ///< The chosen device is absent or failed to open; retried on reconcile().
    };

    /// `base` supplies sample rate, buffer size and channel counts; its
    /// `device_id` is overwritten with the resolved device. `system` must
    /// outlive the session.
    OutputDeviceSession(AudioSystem& system, DeviceConfig base, AudioCallback callback);
    ~OutputDeviceSession();
    OutputDeviceSession(const OutputDeviceSession&) = delete;
    OutputDeviceSession& operator=(const OutputDeviceSession&) = delete;

    /// Adopt `choice` and reconcile. Returns true when the device is running.
    bool open(const OutputChoice& choice);
    /// Stop and release the device. The choice is forgotten.
    void close();
    /// Bring the open device in line with the choice. Returns true when the
    /// device is running afterwards.
    bool reconcile();

    State state() const {
        return state_;
    }
    const OutputChoice& choice() const {
        return choice_;
    }
    /// Id of the device currently open, empty unless `running`.
    const std::string& open_device_id() const {
        return open_id_;
    }
    /// Number of times a device handle was created and started.
    std::uint64_t open_count() const {
        return open_count_;
    }

  private:
    void release_device();
    std::string target_id(bool& present) const;

    AudioSystem& system_;
    DeviceConfig base_;
    AudioCallback callback_;
    OutputChoice choice_;
    std::unique_ptr<AudioDevice> device_;
    std::string open_id_;
    State state_ = State::closed;
    std::uint64_t open_count_ = 0;
    bool has_choice_ = false;
};

} // namespace pulp::audio
