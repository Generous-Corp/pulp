// scripted_ui_prewarm.cpp - one background worker that fills the process-wide
// script bytecode and materialized document caches before an editor opens.

#include <pulp/view/scripted_ui_prewarm.hpp>

#include <pulp/runtime/trace.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#if PULP_HAS_DESIGN_IMPORT
#include <pulp/view/design_sources.hpp>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace pulp::view {
namespace {

// QuickJS's parser recurses on the native stack and the engine allows it 1 MB
// of JS stack; a secondary thread's default (512 KB on macOS) is too small.
constexpr std::size_t kWorkerStackBytes = 8u * 1024u * 1024u;

bool prewarm_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PULP_EDITOR_PREWARM");
        return !(value != nullptr && std::string_view{value} == "0");
    }();
    return on;
}

// The bytecode cache only serves the QuickJS backend.
constexpr bool default_engine_is_quickjs() {
#if (defined(PULP_DEFAULT_ENGINE_V8) && defined(PULP_HAS_V8)) \
    || (defined(PULP_DEFAULT_ENGINE_JSC) && defined(PULP_HAS_JSC))
    return false;
#else
    return true;
#endif
}

class PrewarmWorker {
public:
    static PrewarmWorker& instance() {
        static PrewarmWorker worker;
        return worker;
    }

    void enqueue(ScriptedUiPrewarmRequest request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            queue_.push_back(std::move(request));
            ++stats_.requests;
            stats_.idle = false;
            if (!started_) started_ = start_thread();
            if (!started_) {
                // No thread: drop the work rather than doing it on the
                // caller's thread, which is a host's instantiation path.
                queue_.clear();
                stats_.idle = true;
                return;
            }
        }
        wake_.notify_all();
    }

    ScriptedUiPrewarmStats stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    bool wait_idle(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return idle_.wait_for(lock, timeout, [&] { return stats_.idle; });
    }

    ~PrewarmWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            queue_.clear();
        }
        cancel_.store(true, std::memory_order_relaxed);
        wake_.notify_all();
        join_thread();
    }

private:
    PrewarmWorker() {
        // Construct the caches this worker writes before the worker itself, so
        // static destruction tears the worker down (and joins it) first.
        (void)script_bytecode_cache_stats();
#if PULP_HAS_DESIGN_IMPORT
        (void)materialized_document_cache_stats();
#endif
    }

    void run() {
        for (;;) {
            ScriptedUiPrewarmRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                request = std::move(queue_.front());
                queue_.pop_front();
            }
            process(request);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) stats_.idle = true;
            }
            idle_.notify_all();
        }
    }

    void process(const ScriptedUiPrewarmRequest& request) {
        PULP_TRACE_SCOPE_NAMED("js", "scripted_ui_prewarm");
        // The editor evaluates each through WidgetBridge::load_script(), so
        // compile exactly what that evaluates.
        std::vector<std::string> loaded;
        loaded.reserve(request.scripts.size());
        for (const auto& script : request.scripts)
            loaded.push_back(WidgetBridge::loaded_script_source(script));
        std::size_t compiled = precompile_scripts(loaded, &cancel_);
        std::uint64_t verified = 0;
        std::uint64_t rejected = 0;
#if PULP_HAS_DESIGN_IMPORT
        for (const auto& document : request.materialized_documents) {
            if (cancel_.load(std::memory_order_relaxed)) break;
            const auto before = materialized_document_cache_stats().verifies;
            auto bundle = parse_materialized_browser_document_shared(document);
            if (materialized_document_cache_stats().verifies != before) {
                if (bundle) ++verified;
                else ++rejected;
            }
            if (bundle) compiled += precompile_scripts(runtime_import_whole_scripts(*bundle), &cancel_);
        }
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.scripts_compiled += compiled;
        stats_.documents_verified += verified;
        stats_.documents_rejected += rejected;
    }

#if defined(_WIN32)
    static DWORD WINAPI thread_main(LPVOID self) {
        static_cast<PrewarmWorker*>(self)->run();
        return 0;
    }
    bool start_thread() {
        thread_ = CreateThread(nullptr, kWorkerStackBytes, &PrewarmWorker::thread_main, this,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        return thread_ != nullptr;
    }
    void join_thread() {
        if (!thread_) return;
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    HANDLE thread_ = nullptr;
#else
    static void* thread_main(void* self) {
        static_cast<PrewarmWorker*>(self)->run();
        return nullptr;
    }
    bool start_thread() {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) return false;
        pthread_attr_setstacksize(&attr, kWorkerStackBytes);
#if defined(__APPLE__)
        // Below the UI thread, above background work: an editor may be
        // waiting on this compile.
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
        const bool ok = pthread_create(&thread_, &attr, &PrewarmWorker::thread_main, this) == 0;
        pthread_attr_destroy(&attr);
        has_thread_ = ok;
        return ok;
    }
    void join_thread() {
        if (!has_thread_) return;
        pthread_join(thread_, nullptr);
        has_thread_ = false;
    }
    pthread_t thread_{};
    bool has_thread_ = false;
#endif

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<ScriptedUiPrewarmRequest> queue_;
    ScriptedUiPrewarmStats stats_;
    std::atomic<bool> cancel_{false};
    bool started_ = false;
    bool stopping_ = false;
};

}  // namespace

void prewarm_scripted_ui(ScriptedUiPrewarmRequest request) {
    if (!prewarm_enabled() || !default_engine_is_quickjs()) return;
    if (request.scripts.empty() && request.materialized_documents.empty()) return;
    PrewarmWorker::instance().enqueue(std::move(request));
}

ScriptedUiPrewarmStats scripted_ui_prewarm_stats() {
    return PrewarmWorker::instance().stats();
}

bool wait_for_scripted_ui_prewarm(std::chrono::milliseconds timeout) {
    return PrewarmWorker::instance().wait_idle(timeout);
}

}  // namespace pulp::view
