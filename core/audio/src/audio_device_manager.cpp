/// @file audio_device_manager.cpp
/// AudioDeviceManager — persistence + MIDI hub (item 1.2a).

#include <pulp/audio/audio_device_manager.hpp>
#include <pulp/state/properties_file.hpp>

#include <bitset>
#include <utility>

namespace pulp::audio {

namespace {

int popcount64(uint64_t v) noexcept {
    return static_cast<int>(std::bitset<64>(v).count());
}

}  // namespace

// ── MidiSubscriptionToken ───────────────────────────────────────────

MidiSubscriptionToken::MidiSubscriptionToken(MidiSubscriptionToken&& other) noexcept
    : manager_(std::move(other.manager_)),
      manager_raw_(other.manager_raw_),
      id_(other.id_) {
    other.manager_raw_ = nullptr;
    other.id_ = 0;
}

MidiSubscriptionToken& MidiSubscriptionToken::operator=(MidiSubscriptionToken&& other) noexcept {
    if (this != &other) {
        reset();
        manager_     = std::move(other.manager_);
        manager_raw_ = other.manager_raw_;
        id_          = other.id_;
        other.manager_raw_ = nullptr;
        other.id_          = 0;
    }
    return *this;
}

MidiSubscriptionToken::~MidiSubscriptionToken() {
    reset();
}

void MidiSubscriptionToken::reset() noexcept {
    if (id_ == 0) return;
    if (auto pegged = manager_.lock()) {
        // Manager still alive — pegged keeps it alive until we return.
        if (manager_raw_) {
            manager_raw_->unsubscribe_midi(id_);
        }
    }
    manager_.reset();
    manager_raw_ = nullptr;
    id_ = 0;
}

// ── AudioDeviceManager ──────────────────────────────────────────────

AudioDeviceManager::AudioDeviceManager()
    : lifetime_(std::make_shared<int>(0)) {
}

AudioDeviceManager::~AudioDeviceManager() {
    // Drop lifetime_ first so any outstanding tokens that observe via
    // weak_ptr.lock() see the manager as already dead and skip the
    // unsubscribe call. The token destructor is then a safe no-op.
    lifetime_.reset();
    std::lock_guard<std::mutex> lk(midi_mu_);
    midi_subs_.clear();
}

// ── Persistence ─────────────────────────────────────────────────────

std::optional<DeviceSelection> AudioDeviceManager::load_selection(
    const pulp::state::ApplicationProperties& props) {
    const auto& user = props.user_settings();

    const bool any_present =
        user.contains(adm_keys::kOutputDevice) ||
        user.contains(adm_keys::kInputDevice)  ||
        user.contains(adm_keys::kSampleRate)   ||
        user.contains(adm_keys::kBufferSize)   ||
        user.contains(adm_keys::kOutputChannelMask) ||
        user.contains(adm_keys::kInputChannelMask);
    if (!any_present) return std::nullopt;

    DeviceSelection sel;
    if (auto v = user.get_string(adm_keys::kOutputDevice))      sel.output_device = *v;
    if (auto v = user.get_string(adm_keys::kInputDevice))       sel.input_device  = *v;
    if (auto v = user.get_double(adm_keys::kSampleRate))        sel.sample_rate   = *v;
    if (auto v = user.get_int   (adm_keys::kBufferSize))        sel.buffer_size   = static_cast<int>(*v);
    if (auto v = user.get_int   (adm_keys::kOutputChannelMask)) sel.output_channel_mask = static_cast<uint64_t>(*v);
    if (auto v = user.get_int   (adm_keys::kInputChannelMask))  sel.input_channel_mask  = static_cast<uint64_t>(*v);
    return sel;
}

bool AudioDeviceManager::save_selection(pulp::state::ApplicationProperties& props,
                                        const DeviceSelection& sel) {
    auto& user = props.user_settings();
    user.set_string(adm_keys::kOutputDevice,      sel.output_device);
    user.set_string(adm_keys::kInputDevice,       sel.input_device);
    user.set_double(adm_keys::kSampleRate,        sel.sample_rate);
    user.set_int   (adm_keys::kBufferSize,        sel.buffer_size);
    user.set_int   (adm_keys::kOutputChannelMask, static_cast<int64_t>(sel.output_channel_mask));
    user.set_int   (adm_keys::kInputChannelMask,  static_cast<int64_t>(sel.input_channel_mask));
    // PropertiesFile::save() requires a path. If the caller never
    // load()ed (so path_ is empty), point it at the platform-standard
    // location `user_settings_dir(app_name)/settings.json` so the
    // first save() succeeds without round-tripping through
    // ApplicationProperties::save() (which is void).
    if (user.path().empty()) {
        const std::string dir = pulp::state::ApplicationProperties::user_settings_dir(props.app_name());
        return user.save(dir + "/settings.json");
    }
    return user.save();
}

AudioDeviceManager::ResolveResult AudioDeviceManager::resolve_selection(
    const DeviceSelection& sel,
    const std::vector<DeviceInfo>& available,
    const std::string& fallback_output_id,
    const std::string& fallback_input_id) {
    auto present = [&](const std::string& id) {
        if (id.empty()) return false;
        for (const auto& d : available) {
            if (d.id == id) return true;
        }
        return false;
    };

    ResolveResult r{sel, false, false};
    if (!present(sel.output_device)) {
        r.resolved.output_device = fallback_output_id;
        r.fallback_used_output = true;
    }
    if (!present(sel.input_device)) {
        // Empty input is valid (output-only host); only flag a fallback
        // when the persisted id was non-empty but missing.
        if (!sel.input_device.empty()) r.fallback_used_input = true;
        r.resolved.input_device = fallback_input_id;
        // If both persisted and fallback are empty, no fallback occurred.
        if (sel.input_device.empty() && fallback_input_id.empty()) {
            r.fallback_used_input = false;
        }
    }
    return r;
}

DeviceConfig AudioDeviceManager::selection_to_config(
    const DeviceSelection& sel,
    int default_output_channels,
    int default_input_channels) {
    DeviceConfig cfg;
    cfg.device_id   = sel.output_device;
    cfg.sample_rate = sel.sample_rate > 0.0 ? sel.sample_rate : 48000.0;
    cfg.buffer_size = sel.buffer_size > 0   ? sel.buffer_size : 256;
    cfg.output_channels = sel.output_channel_mask != 0
        ? popcount64(sel.output_channel_mask)
        : default_output_channels;
    cfg.input_channels = sel.input_channel_mask != 0
        ? popcount64(sel.input_channel_mask)
        : default_input_channels;
    return cfg;
}

// ── MIDI hub ────────────────────────────────────────────────────────

MidiSubscriptionToken AudioDeviceManager::subscribe_midi(MidiHandler handler) {
    if (!handler) return {};
    std::lock_guard<std::mutex> lk(midi_mu_);
    const uint64_t id = next_midi_id_++;
    midi_subs_.emplace(id, std::move(handler));
    return MidiSubscriptionToken(
        std::weak_ptr<void>(lifetime_),
        this,
        id);
}

void AudioDeviceManager::dispatch_midi_event(const pulp::midi::MidiEvent& event) {
    // Snapshot under lock so a subscriber callback that itself
    // (un)subscribes doesn't iterate-while-mutating. Copy is cheap —
    // std::function holds a small handler, and the subscriber count
    // for a host is typically O(few).
    std::vector<MidiHandler> handlers;
    {
        std::lock_guard<std::mutex> lk(midi_mu_);
        handlers.reserve(midi_subs_.size());
        for (auto& [_id, fn] : midi_subs_) handlers.push_back(fn);
    }
    for (auto& fn : handlers) {
        if (fn) fn(event);
    }
}

std::size_t AudioDeviceManager::midi_subscriber_count() const {
    std::lock_guard<std::mutex> lk(midi_mu_);
    return midi_subs_.size();
}

void AudioDeviceManager::unsubscribe_midi(uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(midi_mu_);
    midi_subs_.erase(id);
}

// ── CPU load ────────────────────────────────────────────────────────

void AudioDeviceManager::begin_cpu_measure(int num_frames, float sample_rate) {
    std::lock_guard<std::mutex> lk(load_mu_);
    load_.begin(num_frames, sample_rate);
}

void AudioDeviceManager::end_cpu_measure() {
    std::lock_guard<std::mutex> lk(load_mu_);
    load_.end();
}

float AudioDeviceManager::cpu_load() const {
    std::lock_guard<std::mutex> lk(load_mu_);
    return load_.load();
}

float AudioDeviceManager::peak_cpu_load() const {
    std::lock_guard<std::mutex> lk(load_mu_);
    return load_.peak_load();
}

void AudioDeviceManager::reset_peak_cpu_load() {
    std::lock_guard<std::mutex> lk(load_mu_);
    load_.reset_peak();
}

}  // namespace pulp::audio
