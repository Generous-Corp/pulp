// materialized_document_cache.cpp - process-wide reuse of decoded and
// verified materialized browser documents (the runtime-import payload).

#include <pulp/runtime/trace.hpp>
#include <pulp/view/design_sources.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <vector>

namespace pulp::view {
namespace {

// A plug-in process loads one captured document per UI build; a host with
// several Pulp plug-ins holds a few. Bounded so a process that imports many
// distinct documents cannot grow this without limit.
constexpr std::size_t kMaxCachedDocuments = 4;
constexpr std::size_t kMaxCachedBytes = 256u * 1024u * 1024u;

std::size_t bundle_bytes(const ClaudeBundle& bundle) {
    std::size_t bytes = bundle.template_html.size();
    for (const auto& asset : bundle.assets) bytes += asset.data.size();
    return bytes;
}

class MaterializedDocumentCache {
public:
    static MaterializedDocumentCache& instance() {
        static MaterializedDocumentCache cache;
        return cache;
    }

    static bool enabled() {
        static const bool on = [] {
            const char* value = std::getenv("PULP_RUNTIME_IMPORT_CACHE");
            return !(value != nullptr && std::string_view{value} == "0");
        }();
        return on;
    }

    std::shared_ptr<const ClaudeBundle> lookup(const std::string& json) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto hit = find_locked(json)) {
            ++stats_.hits;
            return hit;
        }
        return nullptr;
    }

    // A miss claims `json` for the caller, who verifies it and then calls
    // release(). If another thread holds the claim for the same bytes (a
    // background prewarm started first), wait for its result instead of
    // decoding the same document twice; a waiter that finds the document
    // still absent afterwards (it was rejected, or evicted) claims it itself.
    std::shared_ptr<const ClaudeBundle> lookup_or_claim(const std::string& json,
                                                        bool& claimed) {
        std::unique_lock<std::mutex> lock(mutex_);
        claimed = false;
        for (;;) {
            if (auto hit = find_locked(json)) {
                ++stats_.hits;
                return hit;
            }
            if (!pending_locked(json)) {
                pending_.push_back(&json);
                claimed = true;
                return nullptr;
            }
            ++stats_.waits;
            if (!changed_.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return !pending_locked(json); }))
                return nullptr;  // not claimed: the caller verifies unshared
        }
    }

    void release(const std::string& json) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if (*it == &json) {
                    pending_.erase(it);
                    break;
                }
            }
        }
        changed_.notify_all();
    }

    void note_verify(bool accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.verifies;
        if (!accepted) ++stats_.rejected;
    }

    void store(const std::string& json, std::shared_ptr<const ClaudeBundle> bundle) {
        const std::size_t bytes = json.size() + bundle_bytes(*bundle);
        if (bytes > kMaxCachedBytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_)
            if (entry.input == json) return;  // another thread stored it first
        entries_.push_back({json, std::move(bundle), bytes});
        stats_.bytes += bytes;
        while (!entries_.empty()
               && (entries_.size() > kMaxCachedDocuments || stats_.bytes > kMaxCachedBytes)) {
            stats_.bytes -= entries_.front().bytes;
            entries_.pop_front();
        }
    }

    MaterializedDocumentCacheStats stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto copy = stats_;
        copy.entries = entries_.size();
        return copy;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        stats_ = {};
    }

private:
    struct Entry {
        std::string input;
        std::shared_ptr<const ClaudeBundle> bundle;
        std::size_t bytes = 0;
    };

    std::shared_ptr<const ClaudeBundle> find_locked(const std::string& json) const {
        for (const auto& entry : entries_) {
            if (entry.input.size() == json.size()
                && std::memcmp(entry.input.data(), json.data(), json.size()) == 0)
                return entry.bundle;
        }
        return nullptr;
    }

    // A claim is the claimer's own string, alive until it releases.
    bool pending_locked(const std::string& json) const {
        for (const auto* claimed : pending_)
            if (claimed->size() == json.size()
                && std::memcmp(claimed->data(), json.data(), json.size()) == 0)
                return true;
        return false;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Entry> entries_;
    std::vector<const std::string*> pending_;
    MaterializedDocumentCacheStats stats_;
};

} // namespace

std::shared_ptr<const ClaudeBundle> parse_materialized_browser_document_shared(
    const std::string& json) {
    auto& cache = MaterializedDocumentCache::instance();
    const bool reuse = MaterializedDocumentCache::enabled();
    bool claimed = false;
    if (reuse) {
        if (auto hit = cache.lookup_or_claim(json, claimed)) return hit;
    }
    // Release the claim on every path (a throw inside the decoder included),
    // after the result is stored, so a waiter wakes to find it.
    struct Release {
        MaterializedDocumentCache& cache;
        const std::string& json;
        bool active;
        ~Release() { if (active) cache.release(json); }
    } release{cache, json, claimed};
    std::optional<ClaudeBundle> parsed;
    {
        PULP_TRACE_SCOPE_NAMED("js", "runtime_import_verify");
        parsed = parse_materialized_browser_document(json);
    }
    cache.note_verify(parsed.has_value());
    if (!parsed) return nullptr;
    auto bundle = std::make_shared<const ClaudeBundle>(std::move(*parsed));
    if (reuse) cache.store(json, bundle);
    return bundle;
}

MaterializedDocumentCacheStats materialized_document_cache_stats() {
    return MaterializedDocumentCache::instance().stats();
}

void clear_materialized_document_cache() {
    MaterializedDocumentCache::instance().clear();
}

} // namespace pulp::view
