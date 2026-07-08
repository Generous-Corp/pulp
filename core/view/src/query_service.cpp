#include <pulp/view/query_service.hpp>

#include <string>
#include <utility>

namespace pulp::view {

// ── Registry ────────────────────────────────────────────────────────────────

std::shared_ptr<const runtime::SearchIndex>
QueryService::Registry::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = indices.find(id);
    return it == indices.end() ? nullptr : it->second;
}

bool QueryService::Registry::put_unless_cancelled(
    const std::string& id, std::shared_ptr<const runtime::SearchIndex> index,
    const runtime::CancellationToken& cancel) {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancel.is_cancelled()) return false;
    indices[id] = std::move(index);
    return true;
}

void QueryService::Registry::erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex);
    indices.erase(id);
}

std::size_t QueryService::Registry::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return indices.size();
}

// ── QueryService ────────────────────────────────────────────────────────────

QueryService::QueryService(DeliverFn deliver)
    : deliver_(std::move(deliver)), registry_(std::make_shared<Registry>()) {}

QueryService::~QueryService() {
    // BackgroundJobService's destructor cancels every job and joins the worker,
    // so no job is running once `jobs_` is torn down. Because jobs capture only
    // copies (the DeliverFn and the registry shared_ptr), they are safe even if
    // they were still in flight.
    jobs_.cancel_all();
}

namespace {

// Hand-rolled rather than choc::json: a flat array of non-negative integers is
// trivially correct to serialize and avoids building a choc::value just to throw
// it away, which matters on the search-as-you-type hot path.
std::string indices_to_json(const std::vector<runtime::SearchIndex::Match>& matches) {
    std::string json = "[";
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (i != 0) json.push_back(',');
        json += std::to_string(matches[i].index);
    }
    json.push_back(']');
    return json;
}

}  // namespace

void QueryService::build(std::string dataset_id, std::vector<std::string> items,
                         std::string callback_id) {
    // Supersede any still-running build for this dataset so a rebuild wins and
    // a stale build can't install after it (also lets release() cancel it).
    auto active = active_builds_.find(dataset_id);
    if (active != active_builds_.end()) active->second.cancel();

    runtime::BackgroundJobOptions options;
    options.name = "query-index-build:" + dataset_id;
    options.priority = runtime::BackgroundJobPriority::normal;

    // Capture copies only — never `this` — so the job is self-contained.
    auto handle = jobs_.submit(
        std::move(options),
        [registry = registry_, deliver = deliver_, id = dataset_id, items = std::move(items),
         callback_id = std::move(callback_id)](runtime::BackgroundJobContext& ctx) mutable {
            if (ctx.is_cancelled()) return;

            auto index = std::make_shared<runtime::SearchIndex>();
            index->build(std::move(items), ctx.cancellation_token());
            if (ctx.is_cancelled()) return;

            // Read the count before publishing: once installed, the UI thread
            // may release() the dataset at any moment. put_unless_cancelled
            // installs only if a release() has not cancelled this build, closing
            // the release-vs-build race under the registry lock.
            const auto count = index->size();
            if (!registry->put_unless_cancelled(id, std::move(index), ctx.cancellation_token()))
                return;
            if (!callback_id.empty() && deliver)
                deliver(callback_id, std::to_string(count));
        });

    active_builds_[std::move(dataset_id)] = std::move(handle);
}

void QueryService::query(std::string dataset_id, std::string query_text,
                         std::string callback_id, QueryOptions options) {
    // Search-as-you-type: supersede any still-running query for this dataset so
    // stale results never reach JS and the worker stops scanning early.
    auto active = active_queries_.find(dataset_id);
    if (active != active_queries_.end()) active->second.cancel();

    runtime::SearchQueryOptions search_options;
    search_options.max_results = options.max_results;
    search_options.fuzzy = options.fuzzy;
    search_options.case_sensitive = options.case_sensitive;

    runtime::BackgroundJobOptions job_options;
    job_options.name = "query-index-search:" + dataset_id;
    // Same priority as build so the single worker keeps FIFO submission order:
    // a search submitted right after a build must not jump ahead of it and read
    // a not-yet-installed index. Search-as-you-type responsiveness comes from
    // superseding (cancelling) the previous in-flight search, not from priority.
    job_options.priority = runtime::BackgroundJobPriority::normal;

    auto handle = jobs_.submit(
        std::move(job_options),
        [registry = registry_, deliver = deliver_, id = dataset_id,
         query_text = std::move(query_text), callback_id = std::move(callback_id),
         search_options](runtime::BackgroundJobContext& ctx) mutable {
            if (ctx.is_cancelled()) return;

            auto index = registry->get(id);
            std::vector<runtime::SearchIndex::Match> matches;
            if (index)
                matches = index->query(query_text, search_options, ctx.cancellation_token());

            // A superseded query must not deliver — the newer one owns the
            // callback now.
            if (ctx.is_cancelled()) return;
            if (deliver) deliver(callback_id, indices_to_json(matches));
        });

    active_queries_[std::move(dataset_id)] = std::move(handle);
}

void QueryService::release(const std::string& dataset_id) {
    // Cancel any in-flight query and build BEFORE erasing the registry entry.
    // A build's put_unless_cancelled re-checks this cancellation under the
    // registry lock, so a build racing this release cannot resurrect the
    // dataset after erase().
    if (auto q = active_queries_.find(dataset_id); q != active_queries_.end()) {
        q->second.cancel();
        active_queries_.erase(q);
    }
    if (auto b = active_builds_.find(dataset_id); b != active_builds_.end()) {
        b->second.cancel();
        active_builds_.erase(b);
    }
    registry_->erase(dataset_id);
}

std::size_t QueryService::dataset_count() const {
    return registry_->size();
}

}  // namespace pulp::view
