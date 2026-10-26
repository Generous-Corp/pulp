#include <pulp/gpu_audio/gpu_audio_provider_pool.hpp>

#include "detail/gpu_audio_provider_pool_test_access.hpp"
#include "detail/provider_pool_key.hpp"
#include "detail/wavenet_realtime_channel.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace pulp::gpu_audio {
namespace {

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// FNV-1a over every field that changes the prepared program. Two configs
// with equal keys build identical providers.
class KeyHash {
  public:
    template <typename T> void add(const T& value) noexcept {
        unsigned char bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T));
        for (const auto byte : bytes)
            mix(byte);
    }
    void add_floats(std::span<const float> values) noexcept {
        add(values.size());
        for (const auto value : values)
            add(value);
    }
    std::uint64_t value() const noexcept {
        return hash_;
    }

  private:
    void mix(unsigned char byte) noexcept {
        hash_ = (hash_ ^ byte) * 1099511628211ull;
    }
    std::uint64_t hash_ = 14695981039346656037ull;
};

std::uint64_t program_key(const GpuWaveNetSession::Config& config) noexcept {
    KeyHash hash;
    const auto& d = config.descriptor;
    hash.add(d.block_size);
    hash.add(d.sample_rate);
    hash.add(d.stream_instances);
    hash.add(d.head_scale);
    hash.add(d.weight_count);
    hash.add(d.layers.size());
    for (const auto& layer : d.layers) {
        hash.add(layer.input_size);
        hash.add(layer.condition_size);
        hash.add(layer.channels);
        hash.add(layer.kernel);
        hash.add(layer.head_size);
        hash.add(layer.dilation);
        hash.add(layer.gated);
        hash.add(layer.head_bias);
        hash.add(layer.tanh_activation);
        hash.add(layer.dilations.size());
        for (const auto dilation : layer.dilations)
            hash.add(dilation);
    }
    hash.add_floats(config.weights);
    hash.add(config.slots);
    hash.add(config.completion_policy);
    hash.add(config.completion_wait_ns);
    return hash.value();
}

// Everything program_key() covers, compared exactly on a key match except the
// weights, which are compared by size and an independent hash so a lend never
// copies them. A provider built for one model is never lent to another.
struct ProgramSignature {
    std::vector<std::uint64_t> scalars;
    std::uint64_t weight_count = 0;
    std::uint64_t weight_hash = 0;

    explicit ProgramSignature(const GpuWaveNetSession::Config& config) {
        const auto& d = config.descriptor;
        float scale = d.head_scale;
        std::uint32_t scale_bits = 0;
        std::memcpy(&scale_bits, &scale, sizeof(scale_bits));
        scalars = {d.block_size,
                   d.sample_rate,
                   d.stream_instances,
                   scale_bits,
                   d.weight_count,
                   d.layers.size(),
                   config.slots,
                   static_cast<std::uint64_t>(config.completion_policy),
                   config.completion_wait_ns};
        for (const auto& layer : d.layers) {
            scalars.insert(scalars.end(),
                           {layer.input_size, layer.condition_size, layer.channels, layer.kernel,
                            layer.head_size, layer.dilation, std::uint64_t{layer.gated},
                            std::uint64_t{layer.head_bias}, std::uint64_t{layer.tanh_activation},
                            layer.dilations.size()});
            scalars.insert(scalars.end(), layer.dilations.begin(), layer.dilations.end());
        }
        weight_count = config.weights.size();
        // A different hash from program_key(), so one collision is not both.
        std::uint64_t hash = 0x9e3779b97f4a7c15ull;
        for (const auto weight : config.weights) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &weight, sizeof(bits));
            hash = (hash ^ bits) * 0xff51afd7ed558ccdull;
            hash ^= hash >> 29;
        }
        weight_hash = hash;
    }
    bool operator==(const ProgramSignature&) const = default;
};

} // namespace

struct GpuAudioProviderPool::Impl {
    struct Entry {
        std::uint64_t key = 0;
        std::shared_ptr<const ProgramSignature> signature;
        std::string provider;
        std::uint64_t returned_ns = 0;
        std::unique_ptr<detail::WaveNetRealtimeChannel> channel;
    };
    Config config;
    std::uint64_t (*hash)(const GpuWaveNetSession::Config&) noexcept = &program_key;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    Stats stats;

    // Destroys outside the lock: a provider's release can wait on its device.
    // One deadline across every release: `budget_ns` bounds the whole call,
    // and no release gets zero, which a provider reads as its default.
    void destroy(std::vector<Entry>& doomed, std::uint64_t budget_ns) noexcept {
        const auto start = now_ns();
        const auto deadline = budget_ns > UINT64_MAX - start ? UINT64_MAX : start + budget_ns;
        for (auto& entry : doomed) {
            if (!entry.channel)
                continue;
            const auto now = now_ns();
            const auto remaining = now >= deadline ? std::uint64_t{1} : deadline - now;
            if (!entry.channel->release_bounded(remaining))
                quarantine(std::move(entry.channel));
        }
        doomed.clear();
    }
    void destroy(std::vector<Entry>& doomed) noexcept {
        destroy(doomed, config.release_budget_ns);
    }
    // A provider that could not drain is never freed while the GPU may still
    // touch its memory; it is kept for the life of the process.
    static void quarantine(std::unique_ptr<detail::WaveNetRealtimeChannel> channel) noexcept {
        static std::mutex mutex;
        static auto* held = new std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>>();
        std::lock_guard lock(mutex);
        try {
            held->push_back(std::move(channel));
        } catch (...) {
            (void)channel.release(); // leak rather than free
        }
    }
};

GpuAudioProviderPool::GpuAudioProviderPool() : GpuAudioProviderPool(Config{}) {}

GpuAudioProviderPool::GpuAudioProviderPool(const Config& config) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
}

GpuAudioProviderPool::~GpuAudioProviderPool() {
    clear();
}

void GpuAudioProviderPool::evict_expired() noexcept {
    std::vector<Impl::Entry> doomed;
    {
        std::lock_guard lock(impl_->mutex);
        try {
            doomed.reserve(impl_->entries.size());
        } catch (...) {
            return;
        }
        const auto now = now_ns();
        const auto keep = impl_->config.keep_warm_ns;
        for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
            if (now - it->returned_ns >= keep) {
                doomed.push_back(std::move(*it));
                it = impl_->entries.erase(it);
            } else {
                ++it;
            }
        }
        impl_->stats.evictions += doomed.size();
    }
    impl_->destroy(doomed);
}

void GpuAudioProviderPool::clear() noexcept {
    std::vector<Impl::Entry> doomed;
    {
        std::lock_guard lock(impl_->mutex);
        doomed.swap(impl_->entries);
        impl_->stats.evictions += doomed.size();
    }
    impl_->destroy(doomed);
}

GpuAudioProviderPool::Stats GpuAudioProviderPool::stats() const noexcept {
    std::lock_guard lock(impl_->mutex);
    auto stats = impl_->stats;
    stats.warm_entries = static_cast<std::uint32_t>(impl_->entries.size());
    return stats;
}

std::unique_ptr<detail::WaveNetRealtimeChannel>
GpuAudioProviderPool::acquire(const GpuWaveNetSession::Config& config,
                              const std::string& provider_key,
                              std::uint64_t release_budget_ns) noexcept {
    if (provider_key.empty())
        return nullptr;
    std::vector<Impl::Entry> doomed;
    std::unique_ptr<detail::WaveNetRealtimeChannel> channel;
    try {
        const auto key = impl_->hash(config);
        const ProgramSignature signature(config);
        std::lock_guard lock(impl_->mutex);
        doomed.reserve(impl_->entries.size());
        for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
            if (it->key != key || !detail::provider_pool_key_matches(it->provider, provider_key) ||
                !(*it->signature == signature)) {
                ++it;
                continue;
            }
            // A device lost while parked is never lent; evict it instead.
            if (it->channel->lost()) {
                doomed.push_back(std::move(*it));
                it = impl_->entries.erase(it);
                ++impl_->stats.evictions;
                continue;
            }
            channel = std::move(it->channel);
            impl_->entries.erase(it);
            break;
        }
        ++(channel ? impl_->stats.hits : impl_->stats.misses);
    } catch (...) {
        channel.reset();
    }
    // Lost providers release within the caller's budget (zero: the pool's).
    impl_->destroy(doomed,
                   release_budget_ns != 0 ? release_budget_ns : impl_->config.release_budget_ns);
    return channel;
}

bool GpuAudioProviderPool::give(const GpuWaveNetSession::Config& config,
                                const std::string& provider_key,
                                std::unique_ptr<detail::WaveNetRealtimeChannel>& channel) noexcept {
    if (!channel || impl_->config.capacity == 0 || provider_key.empty())
        return false;
    std::vector<Impl::Entry> doomed;
    try {
        // Everything that allocates or hashes happens before the lock.
        auto entry =
            Impl::Entry{impl_->hash(config), std::make_shared<const ProgramSignature>(config),
                        provider_key, now_ns(), nullptr};
        std::lock_guard lock(impl_->mutex);
        impl_->entries.reserve(impl_->entries.size() + 1);
        doomed.reserve(impl_->entries.size());
        while (impl_->entries.size() >= impl_->config.capacity) {
            doomed.push_back(std::move(impl_->entries.front()));
            impl_->entries.erase(impl_->entries.begin());
            ++impl_->stats.evictions;
        }
        entry.channel = std::move(channel);
        impl_->entries.push_back(std::move(entry));
        ++impl_->stats.returned;
    } catch (...) {
        impl_->destroy(doomed);
        return false;
    }
    impl_->destroy(doomed);
    return true;
}

void detail::GpuAudioProviderPoolTestAccess::set_key_hash(
    GpuAudioProviderPool& pool, std::uint64_t (*hash)(const GpuWaveNetSession::Config&) noexcept) {
    pool.impl_->hash = hash != nullptr ? hash : &program_key;
}
std::unique_ptr<detail::WaveNetRealtimeChannel> detail::GpuAudioProviderPoolTestAccess::acquire(
    GpuAudioProviderPool& pool, const GpuWaveNetSession::Config& config,
    const std::string& provider_key, std::uint64_t release_budget_ns) {
    return pool.acquire(config, provider_key, release_budget_ns);
}
bool detail::GpuAudioProviderPoolTestAccess::give(
    GpuAudioProviderPool& pool, const GpuWaveNetSession::Config& config,
    const std::string& provider_key, std::unique_ptr<detail::WaveNetRealtimeChannel>& channel) {
    return pool.give(config, provider_key, channel);
}

} // namespace pulp::gpu_audio
