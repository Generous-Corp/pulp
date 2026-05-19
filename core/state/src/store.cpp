#include <algorithm>
#include <atomic>
#include <mutex>
#include <pulp/events/event_loop.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/state/store.hpp>
#include <pulp/runtime/assert.hpp>
#include <choc/memory/choc_Endianness.h>

namespace pulp::state {

namespace detail {

struct ListenerRegistry : std::enable_shared_from_this<ListenerRegistry> {
    struct Entry {
        std::uint64_t id = 0;
        ParamChangeCallback callback;
        ListenerThread thread = ListenerThread::Main;
    };

    using EntryList = std::vector<Entry>;
    using SharedEntries = std::shared_ptr<const EntryList>;

    // CoW model: mutators rebuild and swap a new shared_ptr; notify()
    // takes a quick lock only to copy the shared_ptr (refcount bump),
    // then iterates the const snapshot lock-free. The previous design
    // copied the whole vector under the listener mutex on every change,
    // which scaled with listener count; this copies a single pointer.
    mutable std::mutex entries_mutex;
    SharedEntries entries;
    std::atomic<std::uint64_t> next_id{1};
    std::atomic<pulp::events::EventLoop*> main_loop{nullptr};

    // RT → main pending changes. set_value_rt() pushes here (single
    // producer = audio thread); pump_listeners() pops (single consumer
    // = main thread). Capacity is chosen for a few hundred milliseconds
    // of dense automation at 60 Hz UI pump cadence; bigger queues just
    // waste memory while the UI is always close to the head.
    struct RtChange {
        ParamID param_id = 0;
        float value = 0.0f;
    };
    static constexpr std::size_t kRtQueueCapacity = 1024;
    pulp::runtime::SpscQueue<RtChange, kRtQueueCapacity> pending_rt;

    SharedEntries load_snapshot() const {
        std::lock_guard lock(entries_mutex);
        return entries;
    }

    std::uint64_t add(ParamChangeCallback cb, ListenerThread thread) {
        const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(entries_mutex);
        EntryList copy;
        copy.reserve((entries ? entries->size() : 0) + 1);
        if (entries) copy = *entries;
        copy.push_back({id, std::move(cb), thread});
        entries = std::make_shared<const EntryList>(std::move(copy));
        return id;
    }

    void remove(std::uint64_t id) {
        if (id == 0) return;
        std::lock_guard lock(entries_mutex);
        if (!entries) return;
        EntryList copy;
        copy.reserve(entries->size());
        for (const auto& e : *entries) {
            if (e.id != id) copy.push_back(e);
        }
        if (copy.size() == entries->size()) return; // not found
        entries = copy.empty()
            ? SharedEntries{}
            : std::make_shared<const EntryList>(std::move(copy));
    }

    // Re-look-up + invoke at dispatch time so a token reset between
    // EventLoop enqueue and drain cancels the queued call.
    void invoke_if_present(std::uint64_t entry_id, ParamID param_id, float value) {
        auto snap = load_snapshot();
        if (!snap) return;
        for (const auto& entry : *snap) {
            if (entry.id == entry_id) {
                if (entry.callback) entry.callback(param_id, value);
                return;
            }
        }
        // Entry was removed between dispatch and drain — drop the call.
    }

    void notify(ParamID param_id, float value) {
        auto snap = load_snapshot();
        if (!snap || snap->empty()) return;
        auto* loop = main_loop.load(std::memory_order_acquire);
        for (const auto& entry : *snap) {
            if (!entry.callback) continue;
            if (entry.thread == ListenerThread::Audio || loop == nullptr) {
                entry.callback(param_id, value);
            } else {
                // Capture weak_ptr + entry id. At drain time we re-look-up
                // by id, so destroying or reset()-ing the ListenerToken
                // between this enqueue and the EventLoop drain cancels
                // the queued call — the queued lambda no longer holds a
                // stale copy of the callback that could fire after the
                // listener was removed.
                //
                // The shared_ptr<ParamChangeCallback> copy that the lambda
                // would otherwise capture allocates on the firing thread;
                // capturing a weak_ptr is also non-trivial, so this path
                // stays unsafe on the audio thread. Use notify_rt() from
                // format-adapter audio callbacks — see Slice 2 in
                // planning/2026-05-18-rt-safety-and-debug-dx.md.
                std::weak_ptr<ListenerRegistry> weak_self = weak_from_this();
                const auto entry_id = entry.id;
                loop->dispatch([weak_self, entry_id, param_id, value]() {
                    if (auto self = weak_self.lock()) {
                        self->invoke_if_present(entry_id, param_id, value);
                    }
                });
            }
        }
    }

    // Audio-thread fan-out: Audio listeners fire inline (caller asserts
    // RT-safety), Main listeners are deferred via the SPSC queue and
    // drained by pump_listeners() on the main thread. No EventLoop
    // dispatch lambda is allocated on the audio thread.
    void notify_rt(ParamID param_id, float value) {
        auto snap = load_snapshot();
        bool any_main = false;
        if (snap && !snap->empty()) {
            for (const auto& entry : *snap) {
                if (!entry.callback) continue;
                if (entry.thread == ListenerThread::Audio) {
                    entry.callback(param_id, value);
                } else {
                    any_main = true;
                }
            }
        }
        if (any_main) {
            // Lossy push: if the UI hasn't pumped in a while and the
            // queue is saturated, we drop the queued notification. The
            // atomic value store already happened so the UI will pick
            // up the latest on the next pump anyway; only intermediate
            // animation frames are skipped.
            (void) pending_rt.try_push(RtChange{param_id, value});
        }
    }

    std::size_t drain_main_listeners() {
        std::size_t drained = 0;
        while (auto change = pending_rt.try_pop()) {
            ++drained;
            auto snap = load_snapshot();
            if (!snap || snap->empty()) continue;
            for (const auto& entry : *snap) {
                if (entry.callback && entry.thread == ListenerThread::Main) {
                    entry.callback(change->param_id, change->value);
                }
            }
        }
        return drained;
    }
};

} // namespace detail

StateStore::StateStore()
    : registry_(std::make_shared<detail::ListenerRegistry>()) {}

StateStore::~StateStore() {
    // Drop permanent tokens BEFORE the registry shared_ptr goes away so
    // their reset() can lock the weak_ptr cleanly. (Member destruction
    // order would handle this anyway, but being explicit makes the
    // dependency obvious to readers.)
    permanent_listener_tokens_.clear();
}

void StateStore::add_parameter(const ParamInfo& info) {
    auto index = params_.size();
    params_.push_back(info);
    values_.emplace_back(info.range.default_value);
    id_to_index_[info.id] = index;
}

void StateStore::add_group(const ParamGroup& group) {
    groups_.push_back(group);
}

float StateStore::get_value(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return values_[it->second].get();
}

float StateStore::get_modulated(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return values_[it->second].get_modulated();
}

void StateStore::set_mod_offset(ParamID id, float offset) {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) values_[it->second].set_mod_offset(offset);
}

void StateStore::add_mod_offset(ParamID id, float delta) {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) values_[it->second].add_mod_offset(delta);
}

void StateStore::reset_all_mod() {
    for (auto& v : values_) v.reset_mod();
}

void StateStore::set_value(ParamID id, float value) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto& param = params_[it->second];
    float clamped = std::clamp(value, param.range.min, param.range.max);
    values_[it->second].set(clamped);

    // Wait-free fan-out: notify() does a single atomic-shared_ptr load and
    // iterates the const snapshot. Audio listeners run inline; Main
    // listeners route through the installed EventLoop (which allocates;
    // audio-thread callers must use set_value_rt() instead).
    if (registry_) registry_->notify(id, clamped);
}

void StateStore::set_value_rt(ParamID id, float value) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto& param = params_[it->second];
    float clamped = std::clamp(value, param.range.min, param.range.max);
    values_[it->second].set(clamped);
    if (registry_) registry_->notify_rt(id, clamped);
}

void StateStore::set_normalized_rt(ParamID id, float normalized) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto value = params_[it->second].range.denormalize(normalized);
    set_value_rt(id, value);
}

std::size_t StateStore::pump_listeners() {
    if (!registry_) return 0;
    return registry_->drain_main_listeners();
}

float StateStore::get_normalized(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return values_[it->second].get_normalized(params_[it->second].range);
}

void StateStore::set_normalized(ParamID id, float normalized) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto value = params_[it->second].range.denormalize(normalized);
    set_value(id, value);
}

float StateStore::get_default(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return 0.0f;
    return params_[it->second].range.default_value;
}

void StateStore::reset_to_default(ParamID id) {
    set_value(id, get_default(id));
}

void StateStore::reset_all_to_defaults() {
    for (const auto& p : params_) {
        set_value(p.id, p.range.default_value);
    }
}

const ParamInfo* StateStore::info(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return nullptr;
    return &params_[it->second];
}

void StateStore::begin_gesture(ParamID id) {
    if (on_begin_gesture_) on_begin_gesture_(id);
}

void StateStore::end_gesture(ParamID id) {
    if (on_end_gesture_) on_end_gesture_(id);
}

void StateStore::set_main_loop(pulp::events::EventLoop* loop) {
    if (registry_) {
        registry_->main_loop.store(loop, std::memory_order_release);
    }
}

ListenerToken StateStore::add_listener(ParamChangeCallback callback,
                                       ListenerThread thread) {
    if (!registry_) return ListenerToken{};
    const auto id = registry_->add(std::move(callback), thread);
    return ListenerToken(std::weak_ptr<detail::ListenerRegistry>(registry_), id);
}

ListenerToken StateStore::add_audio_listener(ParamChangeCallback callback) {
    return add_listener(std::move(callback), ListenerThread::Audio);
}

void StateStore::remove_listener(ListenerToken& token) {
    token.reset();
}

void StateStore::add_listener(ParamChangeCallback callback) {
    // Legacy permanent-listener entry: behaves like the pre-token API
    // (inline call on the firing thread, no removal). Internally we
    // still go through the registry and stash the token so the modern
    // notify() machinery is the single fan-out path. Migrating callers
    // to the token-returning overload lets them remove the listener.
    auto token = add_listener(std::move(callback), ListenerThread::Audio);
    permanent_listener_tokens_.push_back(std::move(token));
}

// ─── ListenerToken ──────────────────────────────────────────────────────────

ListenerToken::ListenerToken(std::weak_ptr<detail::ListenerRegistry> registry,
                             std::uint64_t id) noexcept
    : registry_(std::move(registry)), id_(id) {}

ListenerToken::ListenerToken(ListenerToken&& other) noexcept
    : registry_(std::move(other.registry_)), id_(other.id_) {
    other.id_ = 0;
}

ListenerToken& ListenerToken::operator=(ListenerToken&& other) noexcept {
    if (this != &other) {
        reset();
        registry_ = std::move(other.registry_);
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

ListenerToken::~ListenerToken() {
    reset();
}

void ListenerToken::reset() noexcept {
    if (id_ == 0) {
        registry_.reset();
        return;
    }
    if (auto reg = registry_.lock()) {
        reg->remove(id_);
    }
    registry_.reset();
    id_ = 0;
}

// ── Serialization ──────────────────────────────────────────────────────────
// Binary format:
//   Header: "PULP" (4 bytes) + version (uint32) + param_count (uint32)
//   Per-param: id (uint32) + value (float)
//   Footer: CRC32 (uint32)

static uint32_t crc32_simple(const uint8_t* data, std::size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::vector<uint8_t> StateStore::serialize() const {
    std::vector<uint8_t> out;
    auto count = static_cast<uint32_t>(params_.size());

    // Header
    out.push_back('P'); out.push_back('U'); out.push_back('L'); out.push_back('P');

    auto write_u32 = [&](uint32_t v) {
        uint8_t buf[4];
        choc::memory::writeLittleEndian(buf, v);
        out.insert(out.end(), buf, buf + 4);
    };

    auto write_float = [&](float v) {
        uint8_t buf[4];
        choc::memory::writeLittleEndian(buf, v);
        out.insert(out.end(), buf, buf + 4);
    };

    write_u32(state_version_);
    write_u32(count);

    // Parameters
    for (std::size_t i = 0; i < params_.size(); ++i) {
        write_u32(params_[i].id);
        write_float(values_[i].get());
    }

    // CRC
    auto crc = crc32_simple(out.data(), out.size());
    write_u32(crc);

    return out;
}

bool StateStore::deserialize(std::span<const uint8_t> data) {
    if (data.size() < 16) return false; // Minimum: header(4) + version(4) + count(4) + crc(4)

    // Check magic
    if (data[0] != 'P' || data[1] != 'U' || data[2] != 'L' || data[3] != 'P')
        return false;

    // Verify CRC
    auto payload_size = data.size() - 4;
    auto stored_crc = choc::memory::readLittleEndian<uint32_t>(data.data() + payload_size);
    auto computed_crc = crc32_simple(data.data(), payload_size);
    if (stored_crc != computed_crc) return false;

    // Read header
    uint32_t version = choc::memory::readLittleEndian<uint32_t>(data.data() + 4);
    if (version > state_version_) return false; // reject future versions we can't parse
    uint32_t count = choc::memory::readLittleEndian<uint32_t>(data.data() + 8);

    // Read parameters
    std::size_t offset = 12;
    for (uint32_t i = 0; i < count && offset + 8 <= payload_size; ++i) {
        ParamID id = choc::memory::readLittleEndian<uint32_t>(data.data() + offset);
        float value = choc::memory::readLittleEndian<float>(data.data() + offset + 4);
        offset += 8;

        // Only set if we know this parameter (forward compatibility)
        auto it = id_to_index_.find(id);
        if (it != id_to_index_.end()) {
            const auto index = it->second;
            const auto& range = params_[index].range;
            values_[index].set(std::clamp(value, range.min, range.max));
        }
    }

    return true;
}

} // namespace pulp::state
