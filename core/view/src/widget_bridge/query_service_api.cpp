// widget_bridge/query_service_api.cpp - off-UI-thread index/search registrations.
//
// Exposes a native index/search service to JS. Heavy work (building an index
// over a large item list, filtering/searching it) runs on a background thread
// via QueryService; only the result — a JSON string of matched item indices —
// is marshaled back onto the UI thread through the same async_exec_results_
// queue that poll_async_results() drains and delivers via
// __dispatch__(callbackId, 'result', payload). The worker never touches the
// view tree, the JS engine, or audio.
//
// JS API:
//   queryIndexBuild(datasetId, itemsArray, callbackId?)
//       Build/replace the index for datasetId from an array of strings. When
//       callbackId is given, receives the item count (as a string) once ready.
//   queryIndexSearch(datasetId, queryText, callbackId, limit?, fuzzy?)
//       Search off-thread; callbackId receives a JSON string of matched item
//       indices, ranked best-first. Supersedes any in-flight search for the
//       same dataset (search-as-you-type). limit defaults to 500 (0 =
//       unlimited); fuzzy defaults to true.
//   queryIndexRelease(datasetId)
//       Drop the dataset's index.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/query_service.hpp>
#include "api_registry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

std::function<void(const std::string&, std::string)> WidgetBridge::make_async_result_sink() {
    auto alive = callback_alive_;
    auto results = async_exec_results_;
    auto mutex = async_exec_mutex_;
    return [alive, results, mutex](const std::string& callback_id, std::string payload) {
        if (!alive || !alive->load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(*mutex);
        results->push_back({callback_id, std::move(payload)});
    };
}

QueryService& WidgetBridge::ensure_query_service() {
    if (!query_service_)
        query_service_ = std::make_shared<QueryService>(make_async_result_sink());
    return *query_service_;
}

void WidgetBridge::register_query_service_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "queryIndexBuild", [this](choc::javascript::ArgumentList args) {
        auto dataset = args.get<std::string>(0, "");
        if (dataset.empty()) return choc::value::Value{};

        std::vector<std::string> items;
        if (args.numArgs > 1 && args[1]) {
            auto& arr = *args[1];
            if (arr.isArray()) {
                items.reserve(arr.size());
                for (uint32_t i = 0; i < arr.size(); ++i)
                    if (arr[i].isString())
                        items.push_back(std::string(arr[i].getString()));
            }
        }

        auto callback = args.get<std::string>(2, "");
        ensure_query_service().build(std::move(dataset), std::move(items), std::move(callback));
        return choc::value::Value{};
    });

    register_bridge_function(api, "queryIndexSearch", [this](choc::javascript::ArgumentList args) {
        auto dataset = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");
        auto callback = args.get<std::string>(2, "");
        if (dataset.empty() || callback.empty()) return choc::value::Value{};

        QueryOptions options;
        const int limit = args.get<int>(3, 500);
        options.max_results = limit < 0 ? std::size_t{500} : static_cast<std::size_t>(limit);
        options.fuzzy = args.get<bool>(4, true);
        options.case_sensitive = args.get<bool>(5, false);

        ensure_query_service().query(std::move(dataset), std::move(text), std::move(callback),
                                     options);
        return choc::value::Value{};
    });

    register_bridge_function(api, "queryIndexRelease", [this](choc::javascript::ArgumentList args) {
        auto dataset = args.get<std::string>(0, "");
        if (!dataset.empty() && query_service_) query_service_->release(dataset);
        return choc::value::Value{};
    });
}

} // namespace pulp::view
