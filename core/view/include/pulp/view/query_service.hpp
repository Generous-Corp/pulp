#pragma once

// QueryService — runs sample-library-scale index/search work OFF the UI thread
// and marshals the results back as JSON strings for delivery on the UI thread.
//
// This is the native "index/service the JS queries" model chosen for R7 (see
// planning/2026-07-06-valdi-lessons-for-pulp.md §4). Rather than standing up a
// second JS heap on a worker thread — which would be engine-specific and would
// need a deep-copy value bridge Pulp does not have — heavy data work is done in
// C++ on a background thread and only the result (a JSON string of matched item
// indices) crosses back to the UI thread.
//
// Thread-safety contract (the important part):
//   - The worker thread only ever touches copied item text, an immutable
//     already-built SearchIndex (via a shared_ptr), and the caller-supplied
//     DeliverFn. It has NO access to the view tree, the JS engine, or audio.
//   - DeliverFn is invoked FROM the worker thread and must itself be
//     thread-safe. The bridge implementation pushes the result onto a
//     mutex-guarded queue that the UI thread drains once per frame; the actual
//     hand-off into JS happens entirely on the UI thread.
//   - The service owns a single BackgroundJobService worker, so builds and
//     queries for a dataset are serialized; a built index is never mutated
//     after publication, so concurrent const queries are safe.
//
// Layering: the reusable, view-independent primitive is `runtime::SearchIndex`.
// QueryService lives in `view/` on purpose — it is the bridge-facing
// orchestrator whose contract (a `callback_id` string + a JSON-string payload)
// is shaped for the JS bridge that is its only consumer. A non-bridge caller
// wanting off-thread search should compose `runtime::SearchIndex` +
// `runtime::BackgroundJobService` directly rather than reach for this glue.

#include <pulp/runtime/background_job.hpp>
#include <pulp/runtime/search_index.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::view {

struct QueryOptions {
    std::size_t max_results = 500;  ///< 0 = unlimited
    bool fuzzy = true;
    bool case_sensitive = false;
};

class QueryService {
public:
    /// Delivers a completed result. `callback_id` is the JS-side callback token
    /// and `json_payload` is a JSON string: for a search, an array of matched
    /// item indices; for a completed build, the item count as a decimal string.
    /// Called ON THE WORKER THREAD, so the implementation must be thread-safe
    /// and must not touch UI state directly — it should enqueue for the UI
    /// thread to drain.
    using DeliverFn =
        std::function<void(const std::string& callback_id, std::string json_payload)>;

    explicit QueryService(DeliverFn deliver);
    ~QueryService();

    QueryService(const QueryService&) = delete;
    QueryService& operator=(const QueryService&) = delete;

    /// Build (or replace) the index for `dataset_id` off the UI thread from
    /// `items`. When `callback_id` is non-empty, the indexed item count (as a
    /// decimal string) is delivered once the index is installed.
    ///
    /// Builds are deliberately fire-and-forget: they are not tracked for
    /// supersession or cancellation (unlike queries). A rebuild simply installs
    /// a newer index that overwrites the previous one; the single FIFO worker
    /// guarantees the later-submitted build wins.
    void build(std::string dataset_id, std::vector<std::string> items,
               std::string callback_id = {});

    /// Search `dataset_id` off the UI thread. The matched item indices (a JSON
    /// array, ranked best-first) are delivered to `callback_id`. Supersedes any
    /// still-running query for the same dataset (search-as-you-type): the older
    /// query is cancelled and never delivers.
    void query(std::string dataset_id, std::string query_text,
               std::string callback_id, QueryOptions options = {});

    /// Drop a dataset's index. Frees the memory once no in-flight job holds it.
    void release(const std::string& dataset_id);

    /// Number of datasets with a currently-installed index (UI-thread view).
    std::size_t dataset_count() const;

private:
    // Registry shared with worker jobs (captured by shared_ptr) so a detached
    // job can safely read an index even if the service is torn down. Guarded by
    // its own mutex; touched by the single worker thread (build install /
    // query lookup) and the UI thread (release / count).
    struct Registry {
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<const runtime::SearchIndex>> indices;

        std::shared_ptr<const runtime::SearchIndex> get(const std::string& id) const;
        // Install `index` for `id` unless `cancel` is already signalled, both
        // checked under the registry lock. release() cancels then erase()s under
        // the same lock, so a build racing a release can never resurrect a
        // released dataset: whichever takes the lock first, the dataset is gone
        // once both complete. Returns true if the index was installed.
        bool put_unless_cancelled(const std::string& id,
                                  std::shared_ptr<const runtime::SearchIndex> index,
                                  const runtime::CancellationToken& cancel);
        void erase(const std::string& id);
        std::size_t size() const;
    };

    DeliverFn deliver_;
    std::shared_ptr<Registry> registry_;
    runtime::BackgroundJobService jobs_;

    // Latest in-flight job handle per dataset, for supersession/cancellation.
    // Touched only on the UI thread (build/query/release are UI-thread entry
    // points). Queries supersede queries; builds supersede builds; release
    // cancels both. Query and build for one dataset are tracked separately so a
    // query never cancels a pending build it depends on.
    std::map<std::string, runtime::BackgroundJobHandle> active_queries_;
    std::map<std::string, runtime::BackgroundJobHandle> active_builds_;
};

}  // namespace pulp::view
