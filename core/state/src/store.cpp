#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>
#include <pulp/events/event_loop.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <cassert>
#include <pulp/state/store.hpp>
#include <pulp/state/state_migration.hpp>
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

    // Reads the current (live atomic) value for a parameter, or nullopt if
    // the parameter is not (or no longer) registered. Installed by the
    // owning StateStore at construction. drain_main_listeners() uses this
    // so Main listeners settle on the param's latest value even when the
    // RT change queue dropped intermediate updates under overflow, and so a
    // change for an absent parameter is skipped rather than delivering a
    // bogus default.
    std::function<std::optional<float>(ParamID)> value_getter;

    using EntryList = std::vector<Entry>;
    using SharedEntries = std::shared_ptr<const EntryList>;

    // CoW model: mutators rebuild and atomically swap a new shared_ptr.
    // Non-RT notification loads that shared snapshot directly; notify_rt()
    // uses the raw RCU snapshot below so it does not take library locks.
    // entries_mutex is only for add/remove serialization on non-RT threads.
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

    // Raw snapshot used only by notify_rt(). std::atomic_load(shared_ptr)
    // may take an internal libstdc++ pthread lock on Linux, which violates
    // the no-lock audio-thread contract. The raw pointer is protected by a
    // tiny RCU-style reader count: add/remove publish a new immutable
    // shared_ptr snapshot, then retain the old snapshot until no RT reader
    // can still hold its raw pointer.
    std::atomic<const EntryList*> rt_entries{nullptr};
    std::atomic<unsigned> rt_readers{0};
    std::vector<SharedEntries> retired_rt_entries;

    struct RtSnapshotLease {
        ListenerRegistry& registry;
        const EntryList* entries = nullptr;

        explicit RtSnapshotLease(ListenerRegistry& r) noexcept
            : registry(r) {
            registry.rt_readers.fetch_add(1, std::memory_order_acq_rel);
            entries = registry.rt_entries.load(std::memory_order_acquire);
        }

        ~RtSnapshotLease() noexcept {
            registry.rt_readers.fetch_sub(1, std::memory_order_acq_rel);
        }

        RtSnapshotLease(const RtSnapshotLease&) = delete;
        RtSnapshotLease& operator=(const RtSnapshotLease&) = delete;
    };

    SharedEntries load_snapshot() const {
        return std::atomic_load_explicit(&entries, std::memory_order_acquire);
    }

    void reclaim_retired_rt_entries_if_idle() {
        if (rt_readers.load(std::memory_order_acquire) == 0) {
            retired_rt_entries.clear();
        }
    }

    void publish_snapshot(SharedEntries next, SharedEntries previous) {
        const EntryList* rt_raw = next.get();
        std::atomic_store_explicit(&entries, std::move(next),
                                   std::memory_order_release);
        rt_entries.store(rt_raw, std::memory_order_release);
        if (previous) retired_rt_entries.push_back(std::move(previous));
        reclaim_retired_rt_entries_if_idle();
    }

    std::uint64_t add(ParamChangeCallback cb, ListenerThread thread) {
        const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(entries_mutex);
        auto current =
            std::atomic_load_explicit(&entries, std::memory_order_acquire);
        EntryList copy;
        copy.reserve((current ? current->size() : 0) + 1);
        if (current) copy = *current;
        copy.push_back({id, std::move(cb), thread});
        publish_snapshot(std::make_shared<const EntryList>(std::move(copy)),
                         std::move(current));
        return id;
    }

    void remove(std::uint64_t id) {
        if (id == 0) return;
        std::lock_guard lock(entries_mutex);
        auto current =
            std::atomic_load_explicit(&entries, std::memory_order_acquire);
        if (!current) return;
        EntryList copy;
        copy.reserve(current->size());
        for (const auto& e : *current) {
            if (e.id != id) copy.push_back(e);
        }
        if (copy.size() == current->size()) return; // not found
        publish_snapshot(
            copy.empty() ? SharedEntries{}
                         : std::make_shared<const EntryList>(std::move(copy)),
            std::move(current));
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
                // format-adapter audio callbacks.
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
        RtSnapshotLease snap(*this);
        bool any_main = false;
        if (snap.entries && !snap.entries->empty()) {
            for (const auto& entry : *snap.entries) {
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

    // Drain the RT->main queue and fan changes out to Main listeners.
    //
    // Contract: Main listeners receive CURRENT-VALUE notifications,
    // coalesced to one call per changed parameter per pump. We drain the
    // whole queue, collect the distinct changed param_ids (in first-seen
    // order), then fire each parameter's Main listeners exactly once with
    // a live value read at drain time. Two reasons:
    //   1. Value coherence — under dense automation the queue drops the
    //      newest pushes when full, so the queued value can be stale.
    //      Reading the live atomic guarantees the latest value; the queue
    //      is only the change-signal.
    //   2. Notification coherence — a burst of N writes to one parameter
    //      would otherwise fire N callbacks all carrying the same live
    //      latest value. Coalescing yields a single, correct notification
    //      per changed parameter.
    // Drain runs on the main thread (not RT), so the small allocation for
    // the distinct-id list is acceptable.
    //
    // @return Number of changes drained from the queue (NOT the number of
    //         callbacks fired — coalescing makes those differ).
    std::size_t drain_main_listeners() {
        std::size_t drained = 0;
        std::vector<ParamID> changed;  // distinct, first-seen order
        while (auto change = pending_rt.try_pop()) {
            ++drained;
            if (std::find(changed.begin(), changed.end(), change->param_id) ==
                changed.end()) {
                changed.push_back(change->param_id);
            }
        }
        if (changed.empty()) return drained;

        auto snap = load_snapshot();
        if (!snap || snap->empty()) return drained;
        for (const ParamID id : changed) {
            // Read the live value once per changed param. If the param is
            // absent (not / no longer registered), skip it rather than
            // delivering a bogus value.
            std::optional<float> current =
                value_getter ? value_getter(id) : std::nullopt;
            if (!current) continue;
            for (const auto& entry : *snap) {
                if (entry.callback && entry.thread == ListenerThread::Main) {
                    entry.callback(id, *current);
                }
            }
        }
        return drained;
    }

    RtListenerQueueTelemetry rt_queue_telemetry() const {
        const auto telemetry = pending_rt.telemetry();
        return {
            .size_approx = telemetry.size_approx,
            .capacity = telemetry.capacity,
            .overflow_count = telemetry.overflow_count,
        };
    }

    void reset_rt_queue_overflow_count() {
        pending_rt.reset_overflow_count();
    }
};

} // namespace detail

namespace {

float constrain_stored_value(const ParamInfo& param, float value) {
    // Preserve the legacy direct-set behavior for untyped parameters, even
    // when their range carries a compatibility step. Explicit semantic kinds
    // opt into canonical integer/toggle/enum quantization on every write path.
    if (param.kind == ParamKind::Continuous) {
        // std::clamp propagates NaN (all comparisons are false), so a NaN/Inf
        // write would otherwise be stored verbatim and reach the DSP and the
        // serialized preset. Sanitize non-finite input to the declared default
        // first — matching the typed path's guard in constrain_param_value.
        if (!std::isfinite(value)) value = param.range.default_value;
        return std::clamp(value, param.range.min, param.range.max);
    }
    return constrain_param_value(param, value);
}

} // namespace

StateStore::StateStore()
    : registry_(std::make_shared<detail::ListenerRegistry>()) {
    // Let the registry read live parameter values when draining queued
    // RT changes to the main thread (see drain_main_listeners). The
    // getter does the lock-free atomic load and runs only on the main
    // thread. It returns nullopt for an unregistered id so the coalesced
    // drain skips changes for absent parameters. `this` outlives registry_
    // (member declaration order in store.hpp puts the parameter storage
    // before registry_, so registry_ is destroyed first).
    registry_->value_getter =
        [this](ParamID id) -> std::optional<float> {
            auto it = id_to_index_.find(id);
            if (it == id_to_index_.end()) return std::nullopt;
            return values_[it->second].get();
        };
}

StateStore::~StateStore() {
    // Drop permanent tokens BEFORE the registry shared_ptr goes away so
    // their reset() can lock the weak_ptr cleanly. (Member destruction
    // order would handle this anyway, but being explicit makes the
    // dependency obvious to readers.)
    permanent_listener_tokens_.clear();
}

void StateStore::add_parameter(const ParamInfo& info) {
    auto index = params_.size();
    // Clamp the declared default into [min, max] so the stored default is
    // consistent with set_value()/reset_to_default() (both clamp). Without this,
    // a parameter declared with an out-of-range default would serialize an
    // out-of-range value that deserialize() then clamps — a silent state
    // round-trip failure. Clamping the ParamInfo too keeps get_default()/info()
    // reporting the same effective default that the store actually holds.
    ParamInfo clamped = info;
    clamped.range.default_value = constrain_stored_value(info, info.range.default_value);
    params_.push_back(clamped);
    values_.emplace_back(clamped.range.default_value);
    id_to_index_[info.id] = index;
    // Cache trigger/momentary params so the audio-thread auto-reset after each
    // block touches only them, allocation-free. A Reset designation implies a
    // trigger (auto_resets()), so it is captured here too.
    if (info.auto_resets()) {
        trigger_indices_.push_back(index);
    }
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
    // base + mod_offset can leave the declared range (e.g. base near max with
    // positive modulation), and the DSP that reads this must never see an
    // out-of-range value. Clamp to the parameter range; a non-finite offset
    // falls back to the default rather than poisoning the audio path.
    const ParamInfo& param = params_[it->second];
    const float modulated = values_[it->second].get_modulated();
    if (!std::isfinite(modulated)) return param.range.default_value;
    return std::clamp(modulated, param.range.min, param.range.max);
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
    float clamped = constrain_stored_value(param, value);
    values_[it->second].set(clamped);

    // Wait-free fan-out: notify() does a single atomic pointer load and
    // iterates the const snapshot. Audio listeners run inline; Main
    // listeners route through the installed EventLoop (which allocates;
    // audio-thread callers must use set_value_rt() instead).
    if (registry_) registry_->notify(id, clamped);
}

void StateStore::set_value_rt(ParamID id, float value) {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return;
    auto& param = params_[it->second];
    float clamped = constrain_stored_value(param, value);
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

RtListenerQueueTelemetry StateStore::rt_listener_queue_telemetry() const {
    if (!registry_) return {};
    return registry_->rt_queue_telemetry();
}

void StateStore::reset_rt_listener_queue_overflow_count() {
    if (registry_) registry_->reset_rt_queue_overflow_count();
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

bool StateStore::reset_triggers_rt() {
    bool any = false;
    for (auto index : trigger_indices_) {
        // Duplicate-ParamID contract: add_parameter keeps every registration in
        // params_/values_ but id_to_index_ resolves an ID to its LATEST slot, so
        // get_value/set_value only ever touch that slot. Skip a cached trigger
        // index that a later same-ID registration has superseded — resetting a
        // dead slot would be a hidden write to a value no reader sees.
        const auto live = id_to_index_.find(params_[index].id);
        if (live == id_to_index_.end() || live->second != index) continue;
        // Write straight to the lock-free atomic: RT-safe, no allocation, no
        // listener dispatch. The host/UI already observed the raised value
        // during the block; this returns the control to its resting default.
        values_[index].set(params_[index].range.default_value);
        any = true;
    }
    return any;
}

const ParamInfo* StateStore::info(ParamID id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return nullptr;
    return &params_[it->second];
}

namespace {

// Detect (and, in debug, assert) a gesture entry point being driven off the
// host main thread. Gesture callbacks forward to host beginEdit/endEdit-style
// undo grouping, which is main-thread-only. When no backend is registered
// (every headless test) this is inert, so tests that legitimately drive
// gestures inline never trip it. Zero cost in release beyond one atomic load
// via has_backend().
bool gesture_off_main_thread() {
    return events::MainThreadDispatcher::has_backend() &&
           !events::MainThreadDispatcher::is_main_thread();
}

} // namespace

void StateStore::begin_gesture(ParamID id) {
    if (gesture_off_main_thread()) {
        gesture_thread_violations_.fetch_add(1, std::memory_order_relaxed);
        assert(false && "StateStore::begin_gesture called off the host main "
                        "thread; use run_gesture_on_main()");
    }
    if (on_begin_gesture_) on_begin_gesture_(id);
}

void StateStore::end_gesture(ParamID id) {
    if (gesture_off_main_thread()) {
        gesture_thread_violations_.fetch_add(1, std::memory_order_relaxed);
        assert(false && "StateStore::end_gesture called off the host main "
                        "thread; use run_gesture_on_main()");
    }
    if (on_end_gesture_) on_end_gesture_(id);
}

void StateStore::run_gesture_on_main(std::function<void()> gesture) {
    if (!gesture) return;
    // Marshal only when a real host backend is registered AND we are off its
    // thread. Otherwise run inline: either we are already on the main thread,
    // or this is a headless context with no backend to post to. call_async
    // swallows exceptions so they cannot escape into a native event queue.
    if (events::MainThreadDispatcher::has_backend() &&
        !events::MainThreadDispatcher::is_main_thread()) {
        events::MainThreadDispatcher::call_async(std::move(gesture));
    } else {
        gesture();
    }
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

bool StateStore::register_state_migration(
    uint32_t from_version,
    uint32_t to_version,
    StateMigrationRegistry::MigrationFn migration) {
    return migrations_.register_migration(from_version, to_version,
                                          std::move(migration));
}

void StateStore::copy_state_migrations_from(const StateStore& source) {
    migrations_ = source.migrations_;
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

    std::vector<uint8_t> migrated_storage;
    if (auto serialized_version = serialized_state_version(data);
        serialized_version.has_value() && *serialized_version != state_version_) {
        if (*serialized_version > state_version_) {
            return false;
        }

        auto migrated = migrations_.migrate(data, state_version_);
        if (migrated.has_value()) {
            migrated_storage = std::move(*migrated);
            data = migrated_storage;
        } else if (migrations_.has_migration_from(*serialized_version)) {
            return false;
        }
    }

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

    constexpr std::size_t header_size = 12;
    constexpr std::size_t param_size = 8;
    if (count > (payload_size - header_size) / param_size ||
        payload_size != header_size + static_cast<std::size_t>(count) * param_size)
        return false;

    // Read parameters
    std::size_t offset = header_size;
    for (uint32_t i = 0; i < count; ++i) {
        ParamID id = choc::memory::readLittleEndian<uint32_t>(data.data() + offset);
        float value = choc::memory::readLittleEndian<float>(data.data() + offset + 4);
        offset += param_size;

        // Only set if we know this parameter (forward compatibility)
        auto it = id_to_index_.find(id);
        if (it != id_to_index_.end()) {
            const auto index = it->second;
            values_[index].set(constrain_stored_value(params_[index], value));
        }
    }

    return true;
}

} // namespace pulp::state
