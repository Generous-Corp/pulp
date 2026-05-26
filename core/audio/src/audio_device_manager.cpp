/// @file audio_device_manager.cpp
/// AudioDeviceManager — persistence + MIDI hub (item 1.2a) + lifecycle
/// / hotplug / recovery (item 1.2b).

#include <pulp/audio/audio_device_manager.hpp>
#include <pulp/state/properties_file.hpp>

#include <algorithm>
#include <bitset>
#include <unordered_set>
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

// DispatchGuard: RAII ref-count that increments `in_flight_` on
// construction (unless the manager is already closed) and decrements
// + notifies `latch_close()` on destruction. Single-purpose helper —
// defined inline here at the top of the TU so every dispatcher in the
// file can construct one on the stack.
class AudioDeviceManager::DispatchGuard {
public:
    explicit DispatchGuard(AudioDeviceManager* mgr) : mgr_(mgr) {
        std::lock_guard<std::mutex> lk(mgr_->latch_mu_);
        if (mgr_->closed_.load(std::memory_order_acquire)) {
            entered_ = false;  // closed; dispatchers bail on entered() == false.
            return;
        }
        ++mgr_->in_flight_;
        entered_ = true;
    }
    ~DispatchGuard() {
        if (!entered_) return;
        std::lock_guard<std::mutex> lk(mgr_->latch_mu_);
        if (--mgr_->in_flight_ == 0) {
            mgr_->latch_cv_.notify_all();
        }
    }
    bool entered() const noexcept { return entered_; }
    DispatchGuard(const DispatchGuard&) = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;
private:
    AudioDeviceManager* mgr_;
    bool entered_ = false;
};

AudioDeviceManager::AudioDeviceManager()
    : lifetime_(std::make_shared<int>(0)) {
}

AudioDeviceManager::~AudioDeviceManager() {
    // Latch the manager first so any in-flight dispatcher returns
    // before we tear state down. After latch_close(), subsequent
    // dispatches are no-ops, and `in_flight_` is guaranteed zero.
    latch_close();

    // Detach from any bound AudioSystem so its callback can no longer
    // reach us. Equivalent to attach_audio_system(nullptr) without
    // the dispatch indirection.
    if (attached_system_) {
        attached_system_->set_device_change_callback(nullptr);
        attached_system_ = nullptr;
    }

    // Drop lifetime_ so any outstanding tokens that observe via
    // weak_ptr.lock() see the manager as already dead and skip the
    // unsubscribe call. The token destructor is then a safe no-op.
    lifetime_.reset();

    {
        std::lock_guard<std::mutex> lk(midi_mu_);
        midi_subs_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(device_change_mu_);
        device_change_subs_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(midi_ep_mu_);
        midi_ep_subs_.clear();
    }
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
    // Allocate the id BEFORE taking midi_mu_. Token ids are globally
    // unique across every subscription map on this manager — see the
    // comment on next_token_id_ in the header. Doing it outside the
    // lock keeps the critical section minimal and means a hypothetical
    // future reordering of subscribe calls can never reuse an id.
    const uint64_t id = next_token_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(midi_mu_);
    midi_subs_.emplace(id, std::move(handler));
    return MidiSubscriptionToken(
        std::weak_ptr<void>(lifetime_),
        this,
        id);
}

void AudioDeviceManager::dispatch_midi_event(const pulp::midi::MidiEvent& event) {
    // After latch_close() this returns immediately, so a host can
    // safely destroy subscribers' owning objects once the latch
    // returns.
    DispatchGuard guard(this);
    if (!guard.entered()) return;

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
    // Token destructors call this for both MIDI-event and MIDI-endpoint
    // subscriptions (they share the MidiSubscriptionToken type for
    // brevity). Try both maps — erase() of a missing key is a no-op.
    {
        std::lock_guard<std::mutex> lk(midi_mu_);
        if (midi_subs_.erase(id) != 0) return;
    }
    {
        std::lock_guard<std::mutex> lk(midi_ep_mu_);
        midi_ep_subs_.erase(id);
    }
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

// ── Lifecycle / hotplug / recovery (1.2b) ───────────────────────────

// ── DeviceChangeToken ───────────────────────────────────────────────

AudioDeviceManager::DeviceChangeToken::DeviceChangeToken(
    DeviceChangeToken&& other) noexcept
    : manager_(std::move(other.manager_)),
      manager_raw_(other.manager_raw_),
      id_(other.id_) {
    other.manager_raw_ = nullptr;
    other.id_ = 0;
}

AudioDeviceManager::DeviceChangeToken&
AudioDeviceManager::DeviceChangeToken::operator=(
    DeviceChangeToken&& other) noexcept {
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

AudioDeviceManager::DeviceChangeToken::~DeviceChangeToken() {
    reset();
}

void AudioDeviceManager::DeviceChangeToken::reset() noexcept {
    if (id_ == 0) return;
    if (auto pegged = manager_.lock()) {
        if (manager_raw_) {
            manager_raw_->unsubscribe_device_change(id_);
        }
    }
    manager_.reset();
    manager_raw_ = nullptr;
    id_ = 0;
}

// ── Hotplug / device change ─────────────────────────────────────────

AudioDeviceManager::DeviceChangeToken
AudioDeviceManager::subscribe_device_changes(DeviceChangeHandler handler) {
    if (!handler) return {};
    // Shared counter — see next_token_id_ comment in the header.
    // DeviceChangeToken routes to its own unsubscribe path, so this
    // particular allocation can't currently collide with MIDI, but
    // using the shared counter guarantees future-proofing if any
    // unsubscribe path ever probes additional maps.
    const uint64_t id = next_token_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(device_change_mu_);
    device_change_subs_.emplace(id, std::move(handler));
    return DeviceChangeToken(
        std::weak_ptr<void>(lifetime_),
        this,
        id);
}

std::size_t AudioDeviceManager::device_change_subscriber_count() const {
    std::lock_guard<std::mutex> lk(device_change_mu_);
    return device_change_subs_.size();
}

void AudioDeviceManager::unsubscribe_device_change(uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(device_change_mu_);
    device_change_subs_.erase(id);
}

void AudioDeviceManager::attach_audio_system(AudioSystem* system) {
    // If we were already bound, clear the old callback so the previous
    // AudioSystem stops talking to us.
    if (attached_system_ && attached_system_ != system) {
        attached_system_->set_device_change_callback(nullptr);
    }
    attached_system_ = system;
    if (!system) return;

    // Bridge the AudioSystem's "the list changed" callback into a
    // structured DeviceChangeEvent the manager can fan out. We can't
    // know "what" changed from the CoreAudio side without diffing
    // (which is the subscriber's job if they care), so the kind is
    // `Unknown` here.
    system->set_device_change_callback([this, system]() {
        DeviceChangeEvent ev;
        ev.kind    = DeviceChangeKind::Unknown;
        ev.devices = system->enumerate_devices();
        dispatch_device_change(ev);
    });
}

void AudioDeviceManager::dispatch_device_change(const DeviceChangeEvent& event) {
    DispatchGuard guard(this);
    if (!guard.entered()) return;  // closed; drop event.

    std::vector<DeviceChangeHandler> handlers;
    {
        std::lock_guard<std::mutex> lk(device_change_mu_);
        handlers.reserve(device_change_subs_.size());
        for (auto& [_id, fn] : device_change_subs_) handlers.push_back(fn);
    }
    for (auto& fn : handlers) {
        if (fn) fn(event);
    }
}

// ── Default-device change ───────────────────────────────────────────

void AudioDeviceManager::set_default_device_change_handler(
    DefaultDeviceChangeHandler handler) {
    std::lock_guard<std::mutex> lk(default_dev_mu_);
    default_dev_handler_ = std::move(handler);
}

void AudioDeviceManager::dispatch_default_device_change(
    bool is_input, const std::string& new_device_id) {
    DispatchGuard guard(this);
    if (!guard.entered()) return;

    DefaultDeviceChangeHandler handler;
    {
        std::lock_guard<std::mutex> lk(default_dev_mu_);
        handler = default_dev_handler_;
    }
    if (handler) handler(is_input, new_device_id);
}

// ── Sample-rate change ──────────────────────────────────────────────

void AudioDeviceManager::set_sample_rate_change_handler(
    SampleRateChangeHandler handler) {
    std::lock_guard<std::mutex> lk(sr_mu_);
    sr_handler_ = std::move(handler);
}

void AudioDeviceManager::dispatch_sample_rate_change(double new_rate) {
    DispatchGuard guard(this);
    if (!guard.entered()) return;

    last_sr_.store(new_rate, std::memory_order_relaxed);
    sr_change_count_.fetch_add(1, std::memory_order_relaxed);

    SampleRateChangeHandler handler;
    {
        std::lock_guard<std::mutex> lk(sr_mu_);
        handler = sr_handler_;
    }
    if (handler) handler(new_rate);
}

std::uint64_t AudioDeviceManager::sample_rate_change_count() const {
    return sr_change_count_.load(std::memory_order_relaxed);
}

double AudioDeviceManager::last_sample_rate() const {
    return last_sr_.load(std::memory_order_relaxed);
}

// ── xrun tracking ───────────────────────────────────────────────────

void AudioDeviceManager::bump_xrun_counter(std::uint64_t delta) {
    xrun_counter_.fetch_add(delta, std::memory_order_relaxed);
}

std::uint64_t AudioDeviceManager::xrun_count() const {
    return xrun_counter_.load(std::memory_order_relaxed);
}

void AudioDeviceManager::reset_xrun_counter() {
    xrun_counter_.store(0, std::memory_order_relaxed);
}

// ── MIDI endpoint tracking ──────────────────────────────────────────

MidiSubscriptionToken AudioDeviceManager::subscribe_midi_endpoints(
    MidiEndpointChangeHandler handler) {
    if (!handler) return {};
    // Globally unique across midi_subs_ AND midi_ep_subs_; see
    // next_token_id_ comment in the header. This is what stops
    // unsubscribe_midi() from erasing the wrong subscriber when a
    // MIDI handler and an endpoint handler happen to share an id
    // (previously both counters started at 1).
    const uint64_t id = next_token_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(midi_ep_mu_);
    midi_ep_subs_.emplace(id, std::move(handler));
    // Reuse MidiSubscriptionToken; its unsubscribe routes via
    // unsubscribe_midi_endpoint() because we mark the id with the
    // bit-31 channel — but that pattern bloats the token surface for
    // little gain. Instead, both unsubscribe paths run a quick erase()
    // on both maps with their respective mutexes. erase() of a missing
    // key is a no-op, so a token from subscribe_midi_endpoints() that
    // gets routed to unsubscribe_midi() is harmless: the wrong map is
    // checked, erase returns 0, lookup of the right map happens next
    // call. To keep semantics clear, we make the token always call
    // unsubscribe_midi(), and unsubscribe_midi() tries both maps.
    return MidiSubscriptionToken(
        std::weak_ptr<void>(lifetime_),
        this,
        id);
}

std::size_t AudioDeviceManager::midi_endpoint_subscriber_count() const {
    std::lock_guard<std::mutex> lk(midi_ep_mu_);
    return midi_ep_subs_.size();
}

void AudioDeviceManager::unsubscribe_midi_endpoint(uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(midi_ep_mu_);
    midi_ep_subs_.erase(id);
}

void AudioDeviceManager::set_midi_endpoints(std::vector<MidiEndpoint> endpoints) {
    // Compute additions / removals before we publish, then dispatch
    // outside the lock.
    std::vector<MidiEndpointChange> changes;
    {
        std::lock_guard<std::mutex> lk(midi_ep_mu_);

        std::unordered_set<std::string> old_ids;
        old_ids.reserve(midi_endpoints_.size());
        for (const auto& ep : midi_endpoints_) old_ids.insert(ep.id);

        std::unordered_set<std::string> new_ids;
        new_ids.reserve(endpoints.size());
        for (const auto& ep : endpoints) new_ids.insert(ep.id);

        // Removals: in old, not in new.
        for (const auto& ep : midi_endpoints_) {
            if (new_ids.find(ep.id) == new_ids.end()) {
                changes.push_back({MidiEndpointChangeKind::Removed, ep});
            }
        }
        // Additions: in new, not in old.
        for (const auto& ep : endpoints) {
            if (old_ids.find(ep.id) == old_ids.end()) {
                changes.push_back({MidiEndpointChangeKind::Added, ep});
            }
        }

        midi_endpoints_ = std::move(endpoints);
    }

    if (changes.empty()) return;

    DispatchGuard guard(this);
    if (!guard.entered()) return;

    std::vector<MidiEndpointChangeHandler> handlers;
    {
        std::lock_guard<std::mutex> lk(midi_ep_mu_);
        handlers.reserve(midi_ep_subs_.size());
        for (auto& [_id, fn] : midi_ep_subs_) handlers.push_back(fn);
    }
    for (auto& fn : handlers) {
        if (!fn) continue;
        for (const auto& change : changes) fn(change);
    }
}

std::vector<AudioDeviceManager::MidiEndpoint>
AudioDeviceManager::midi_endpoints() const {
    std::lock_guard<std::mutex> lk(midi_ep_mu_);
    return midi_endpoints_;
}

// ── Concurrent-callback shutdown ────────────────────────────────────

void AudioDeviceManager::latch_close() {
    // Mark closed first so any dispatcher that wins the race observes
    // the close before it increments `in_flight_`.
    closed_.store(true, std::memory_order_release);

    std::unique_lock<std::mutex> lk(latch_mu_);
    latch_cv_.wait(lk, [this] { return in_flight_ == 0; });
}

bool AudioDeviceManager::is_closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

}  // namespace pulp::audio
