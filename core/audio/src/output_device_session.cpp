#include <pulp/audio/output_device_session.hpp>

#include <algorithm>
#include <utility>

namespace pulp::audio {

OutputDeviceSession::OutputDeviceSession(AudioSystem& system, DeviceConfig base,
                                         AudioCallback callback)
    : system_(system), base_(std::move(base)), callback_(std::move(callback)) {}

OutputDeviceSession::~OutputDeviceSession() {
    release_device();
}

bool OutputDeviceSession::open(const OutputChoice& choice) {
    choice_ = choice;
    has_choice_ = true;
    return reconcile();
}

void OutputDeviceSession::close() {
    release_device();
    choice_ = {};
    has_choice_ = false;
    state_ = State::closed;
}

void OutputDeviceSession::release_device() {
    if (!device_)
        return;
    // stop() returns only after any in-flight block has finished; the handle is
    // closed and destroyed before any replacement exists.
    device_->stop();
    device_->close();
    device_.reset();
    open_id_.clear();
}

std::string OutputDeviceSession::target_id(bool& present) const {
    std::string id = choice_.follow_default ? system_.default_output_device().id
                                            : choice_.device_id;
    present = false;
    if (id.empty())
        return id;
    const auto devices = system_.enumerate_devices();
    present = std::any_of(devices.begin(), devices.end(), [&](const DeviceInfo& d) {
        return d.id == id && d.max_output_channels > 0;
    });
    return id;
}

bool OutputDeviceSession::reconcile() {
    if (!has_choice_)
        return false;

    bool present = false;
    const std::string id = target_id(present);
    if (!present) {
        release_device();
        state_ = State::missing;
        return false;
    }

    if (device_ && open_id_ == id && device_->is_open() && device_->is_running()) {
        state_ = State::running;
        return true;
    }

    release_device();
    auto fresh = system_.create_device(id);
    if (!fresh) {
        state_ = State::missing;
        return false;
    }
    DeviceConfig config = base_;
    config.device_id = id;
    if (!fresh->open(config)) {
        state_ = State::missing;
        return false;
    }
    if (!fresh->start(callback_)) {
        fresh->close();
        state_ = State::missing;
        return false;
    }
    device_ = std::move(fresh);
    open_id_ = id;
    ++open_count_;
    state_ = State::running;
    return true;
}

} // namespace pulp::audio
