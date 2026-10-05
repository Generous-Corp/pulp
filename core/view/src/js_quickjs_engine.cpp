// QuickJS backend for the Pulp JS engine abstraction.
// Wraps choc::javascript::Context (QuickJS) — the original and portable default.

// Include QuickJS implementation — must appear in exactly one translation unit
#include <choc/javascript/choc_javascript_QuickJS.h>
#include <choc/javascript/choc_javascript_Console.h>

#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/view/js_engine.hpp>
#include <atomic>
#include <functional>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string_view>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pulp::view {

// ── QuickJS stack size increase ─────────────────────────────────────────────
// The web-compat prelude + deep JS↔C++ interleaving during DOM operations
// (appendChild → _reparentNative → native bridge → back to JS) exhausts
// QuickJS's default 256KB JS stack. We increase it to 1MB.
//
static void set_quickjs_stack_size(
    choc::javascript::quickjs::QuickJSContext& context, size_t size) {
    if (context.runtime)
        JS_SetMaxStackSize(context.runtime, size);
}

// Drive QuickJS's pending-job queue ourselves: CHOC's pumpMessageLoop() is
// an empty no-op for QuickJS (issue #746), so queueMicrotask / Promise.then /
// async-await never drain unless we run JS_ExecutePendingJob to completion.
// It returns 1 when it ran a job, 0 when the queue is empty, and a negative
// value on a JS exception inside the job; stop on either terminal state.
//
// The job count is capped rather than looping forever. Production callers
// (`WidgetBridge::service_frame_callbacks`, design-import drain) run this
// synchronously on the UI thread, and a self-rearming microtask
// (`queueMicrotask(step)` inside `step`) would otherwise hang the host. The
// cap sits far above any legitimate boot chain (~5K for React 18 + Babel +
// bundled prelude) and logs a warning when it fires so the runaway is visible.
namespace {
constexpr int kQuickJsPumpJobCap = 1'000'000;

namespace qjs = choc::javascript::quickjs;

class BoundedJsonWriter {
public:
    BoundedJsonWriter(qjs::QuickJSContext& owner, std::size_t limit)
        : owner_(owner), context_(owner.context), limit_(limit) {
        output_.reserve(std::min<std::size_t>(limit, 4096));
    }

    std::string write(qjs::JSValueConst value) {
        write_value(value, 0);
        return std::move(output_);
    }

private:
    static constexpr std::size_t kMaxDepth = 32;

    qjs::QuickJSContext& owner_;
    qjs::JSContext* context_;
    std::size_t limit_;
    std::string output_;
    std::unordered_set<const void*> active_objects_;

    [[noreturn]] void result_too_large() const {
        throw std::length_error(
            "Runtime.evaluate result exceeds the "
            + std::to_string(limit_) + "-byte limit");
    }

    [[noreturn]] void throw_pending_exception() {
        auto exception = owner_.takeValue(qjs::JS_GetException(context_));
        exception.throwIfError();
        throw std::runtime_error("QuickJS operation failed");
    }

    void check_interrupted() {
        if (owner_.shouldCancel.exchange(false, std::memory_order_acq_rel))
            throw std::runtime_error("interrupted");
    }

    void append(std::string_view text) {
        check_interrupted();
        if (text.size() > limit_ - output_.size())
            result_too_large();
        output_.append(text);
    }

    void append_byte(char value) {
        check_interrupted();
        if (output_.size() == limit_)
            result_too_large();
        output_.push_back(value);
    }

    void append_codepoint(std::uint32_t value) {
        if (value == '"' || value == '\\') {
            append_byte('\\');
            append_byte(static_cast<char>(value));
        } else if (value == '\b') {
            append("\\b");
        } else if (value == '\f') {
            append("\\f");
        } else if (value == '\n') {
            append("\\n");
        } else if (value == '\r') {
            append("\\r");
        } else if (value == '\t') {
            append("\\t");
        } else if (value < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            char escaped[] = {'\\', 'u', '0', '0',
                              hex[(value >> 4) & 0xf], hex[value & 0xf]};
            append(std::string_view(escaped, sizeof(escaped)));
        } else if (value <= 0x7f) {
            append_byte(static_cast<char>(value));
        } else if (value <= 0x7ff) {
            char encoded[] = {
                static_cast<char>(0xc0 | (value >> 6)),
                static_cast<char>(0x80 | (value & 0x3f))};
            append(std::string_view(encoded, sizeof(encoded)));
        } else if (value <= 0xffff) {
            char encoded[] = {
                static_cast<char>(0xe0 | (value >> 12)),
                static_cast<char>(0x80 | ((value >> 6) & 0x3f)),
                static_cast<char>(0x80 | (value & 0x3f))};
            append(std::string_view(encoded, sizeof(encoded)));
        } else {
            char encoded[] = {
                static_cast<char>(0xf0 | (value >> 18)),
                static_cast<char>(0x80 | ((value >> 12) & 0x3f)),
                static_cast<char>(0x80 | ((value >> 6) & 0x3f)),
                static_cast<char>(0x80 | (value & 0x3f))};
            append(std::string_view(encoded, sizeof(encoded)));
        }
    }

    void write_string(const qjs::JSString& string) {
        append_byte('"');
        for (std::uint32_t i = 0; i < string.len; ++i) {
            std::uint32_t codepoint = string.is_wide_char
                ? string.u.str16[i]
                : string.u.str8[i];
            if (string.is_wide_char
                && codepoint >= 0xd800 && codepoint <= 0xdbff
                && i + 1 < string.len) {
                const auto low = static_cast<std::uint32_t>(string.u.str16[i + 1]);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codepoint =
                        0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    ++i;
                }
            }
            if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
                constexpr char hex[] = "0123456789abcdef";
                char escaped[] = {
                    '\\', 'u',
                    hex[(codepoint >> 12) & 0xf],
                    hex[(codepoint >> 8) & 0xf],
                    hex[(codepoint >> 4) & 0xf],
                    hex[codepoint & 0xf]};
                append(std::string_view(escaped, sizeof(escaped)));
            } else {
                append_codepoint(codepoint);
            }
        }
        append_byte('"');
    }

    void write_value(qjs::JSValueConst value, std::size_t depth) {
        if (qjs::JS_IsNull(value) || qjs::JS_IsUndefined(value)) {
            append("null");
            return;
        }
        if (qjs::JS_IsBool(value)) {
            append(qjs::JS_ToBool(context_, value) ? "true" : "false");
            return;
        }
        if (qjs::JS_IsNumber(value)) {
            double number = 0.0;
            if (qjs::JS_ToFloat64(context_, &number, value) != 0
                || !std::isfinite(number)) {
                append("null");
                return;
            }
            char text[64];
            const auto converted = std::to_chars(
                std::begin(text), std::end(text), number,
                std::chars_format::general);
            if (converted.ec != std::errc{})
                throw std::runtime_error("could not serialize evaluation number");
            append(std::string_view(
                text, static_cast<std::size_t>(converted.ptr - text)));
            return;
        }
        if (qjs::JS_IsString(value)) {
            write_string(*static_cast<qjs::JSString*>(value.u.ptr));
            return;
        }
        if (!qjs::JS_IsObject(value)) {
            append("null");
            return;
        }
        if (depth >= kMaxDepth)
            throw std::runtime_error(
                "Runtime.evaluate result exceeds the 32-level depth limit");

        const void* identity = value.u.ptr;
        if (!active_objects_.insert(identity).second)
            throw std::runtime_error("Runtime.evaluate result contains a cycle");
        struct ActiveScope {
            std::unordered_set<const void*>& objects;
            const void* identity;
            ~ActiveScope() { objects.erase(identity); }
        } active_scope{active_objects_, identity};

        auto* object = static_cast<qjs::JSObject*>(value.u.ptr);
        if (!object || !object->shape)
            throw std::runtime_error("Runtime.evaluate result has an invalid object");
        // Accept direct arrays, but reject proxies and other exotic objects.
        // Their ownKeys hooks can allocate or execute arbitrarily before the
        // serializer sees a count, defeating this component's resource bound.
        const bool is_array = object->class_id == qjs::JS_CLASS_ARRAY;
        if (is_array) {
            auto length_value = owner_.takeValue(
                qjs::JS_GetPropertyStr(context_, value, "length"));
            length_value.throwIfError();
            std::uint32_t length = 0;
            if (qjs::JS_ToUint32(context_, &length, length_value.get()) != 0)
                throw_pending_exception();
            if (length > limit_)
                result_too_large();
            append_byte('[');
            for (std::uint32_t i = 0; i < length; ++i) {
                if (i != 0) append_byte(',');
                auto element = owner_.takeValue(
                    qjs::JS_GetPropertyUint32(context_, value, i));
                element.throwIfError();
                write_value(element.get(), depth + 1);
            }
            append_byte(']');
            return;
        }

        if (object->is_exotic)
            throw std::runtime_error(
                "Runtime.evaluate result contains an unsupported exotic object");

        // JS_GetOwnPropertyNames allocates its complete descriptor array. For
        // ordinary objects the shape exposes a conservative live-property
        // count, so reject before that allocation when even the minimum JSON
        // punctuation would exceed the caller's byte budget.
        const auto shape_property_count = object->shape->prop_count;
        const auto deleted_property_count = object->shape->deleted_prop_count;
        if (shape_property_count < 0 || deleted_property_count < 0
            || deleted_property_count > shape_property_count)
            throw std::runtime_error("Runtime.evaluate result has an invalid object shape");
        const auto live_property_count = static_cast<std::size_t>(
            shape_property_count - deleted_property_count);
        const auto property_budget = limit_ / 4 + (limit_ % 4 != 0 ? 1 : 0);
        if (live_property_count > property_budget)
            result_too_large();

        qjs::JSPropertyEnum* properties = nullptr;
        std::uint32_t property_count = 0;
        if (qjs::JS_GetOwnPropertyNames(
                context_, &properties, &property_count, value,
                qjs::JS_GPN_STRING_MASK | qjs::JS_GPN_ENUM_ONLY) != 0) {
            throw_pending_exception();
        }
        struct PropertyScope {
            qjs::JSContext* context;
            qjs::JSPropertyEnum* properties;
            std::uint32_t count;
            ~PropertyScope() {
                for (std::uint32_t i = 0; i < count; ++i)
                    qjs::JS_FreeAtom(context, properties[i].atom);
                if (properties)
                    qjs::js_free(context, properties);
            }
        } property_scope{context_, properties, property_count};
        if (property_count > limit_)
            result_too_large();

        append_byte('{');
        for (std::uint32_t i = 0; i < property_count; ++i) {
            if (i != 0) append_byte(',');
            auto key = owner_.takeValue(
                qjs::JS_AtomToValue(context_, properties[i].atom));
            key.throwIfError();
            write_string(*static_cast<qjs::JSString*>(key.get().u.ptr));
            append_byte(':');
            auto member = owner_.takeValue(
                qjs::JS_GetProperty(context_, value, properties[i].atom));
            member.throwIfError();
            write_value(member.get(), depth + 1);
        }
        append_byte('}');
    }
};
}

// Pulp #3206 — Promise-rejection tracker.
//
// QuickJS does not call JS-side `addEventListener("unhandledrejection", ...)`
// handlers; if a Promise rejects with no `.catch`, the rejection is silently
// dropped. The most damaging case is the `new Promise(async (resolve, reject) => { ... })`
// anti-pattern: a sync throw inside the executor rejects the *inner* async
// function's promise (not the outer Promise), and the outer Promise sits in
// pending forever. Three.js's WebGPURenderer.init() uses this exact pattern,
// which is why iOS-D.3c's cube never rendered — the silent throw blocked the
// init promise indefinitely. See issue #3206 for the full reproducer.
//
// JS_SetHostPromiseRejectionTracker is QuickJS's hook for this: it fires
// whenever a Promise rejection is created (is_handled=0) or later attached
// to a handler (is_handled=1). We log the unhandled case to runtime::log_error
// so the symptom becomes a visible error line instead of an invisible hang.
// The handled-later case is benign (callers caught the rejection in time) so
// we drop it.
static void pulp_quickjs_promise_rejection_tracker(
        choc::javascript::quickjs::JSContext* ctx,
        choc::javascript::quickjs::JSValueConst /*promise*/,
        choc::javascript::quickjs::JSValueConst reason,
        int is_handled,
        void* /*opaque*/) {
    if (is_handled) return;  // someone attached .catch — not unhandled anymore
    const char* msg = choc::javascript::quickjs::JS_ToCString(ctx, reason);
    pulp::runtime::log_error(
        "PULP_QJS_UNHANDLED_REJECTION: {}",
        msg ? msg : "<no string representation>");
    if (msg) choc::javascript::quickjs::JS_FreeCString(ctx, msg);
    // Try to also surface the .stack so the actual call site is logged when
    // the reason is an Error. JS_GetPropertyStr returns an exception JSValue
    // on missing prop, which is fine — we just check and free.
    choc::javascript::quickjs::JSValue stack =
        choc::javascript::quickjs::JS_GetPropertyStr(ctx, reason, "stack");
    if (!choc::javascript::quickjs::JS_IsUndefined(stack)
        && !choc::javascript::quickjs::JS_IsException(stack)) {
        const char* stack_msg = choc::javascript::quickjs::JS_ToCString(ctx, stack);
        if (stack_msg) {
            pulp::runtime::log_error("PULP_QJS_UNHANDLED_REJECTION_STACK: {}", stack_msg);
            choc::javascript::quickjs::JS_FreeCString(ctx, stack_msg);
        }
    }
    choc::javascript::quickjs::JS_FreeValue(ctx, stack);
}

static void install_quickjs_rejection_tracker(qjs::QuickJSContext& context) {
    if (!context.runtime) return;
    choc::javascript::quickjs::JS_SetHostPromiseRejectionTracker(
        context.runtime, &pulp_quickjs_promise_rejection_tracker, nullptr);
}

static void pump_quickjs_jobs(qjs::QuickJSContext& context) {
    if (!context.runtime) return;
    choc::javascript::quickjs::JSContext* pctx = nullptr;
    for (int executed = 0; executed < kQuickJsPumpJobCap; ++executed) {
        int rc = JS_ExecutePendingJob(context.runtime, &pctx);
        if (rc <= 0) return;  // 0 = empty queue, <0 = JS exception inside job
    }
    pulp::runtime::log_warn(
        "QuickJS pump_message_loop hit the {}-job safety cap — likely a "
        "self-rearming microtask (queueMicrotask/Promise.then loop) in JS. "
        "Returning to avoid hanging the UI thread; the runaway queue is "
        "left pending.", kQuickJsPumpJobCap);
}

static std::string_view logging_level_name(choc::javascript::LoggingLevel level) {
    switch (level) {
        case choc::javascript::LoggingLevel::log:   return "log";
        case choc::javascript::LoggingLevel::info:  return "info";
        case choc::javascript::LoggingLevel::warn:  return "warn";
        case choc::javascript::LoggingLevel::error: return "error";
        case choc::javascript::LoggingLevel::debug: return "debug";
        default: return "log";
    }
}

namespace {

// evaluate_script() is only called for whole scripts (bridge preludes, bundle
// payloads, a document's inline <script> blocks); per-frame expression traffic
// goes through evaluate(). Below this size a compile costs about what the
// lookup and bytecode read do, so it is not worth an entry.
constexpr std::size_t kMinCacheableScriptBytes = 2 * 1024;
// The distinct whole scripts of a few loaded UI builds: preludes, a runtime
// per plug-in build, and each document's inline scripts. Bounded so an app
// that evaluates many distinct scripts cannot grow this without limit.
constexpr std::size_t kMaxCachedScripts = 64;
constexpr std::size_t kMaxCachedBytes = 96u * 1024u * 1024u;

class ScriptBytecodeCache {
public:
    static ScriptBytecodeCache& instance() {
        static ScriptBytecodeCache cache;
        return cache;
    }

    static bool enabled() {
        static const bool on = [] {
            const char* value = std::getenv("PULP_JS_BYTECODE_CACHE");
            return !(value != nullptr && std::string_view{value} == "0");
        }();
        return on;
    }

    enum class Claim { hit, claimed, busy };

    // Copies the bytecode out under the lock, so a concurrent eviction never
    // frees bytes a reader is still deserializing. A miss claims the source:
    // the caller compiles it and must call finish() (or abandon()). When
    // another thread already holds the claim for byte-identical source -- a
    // background precompile started before the editor asked -- `wait` blocks
    // until that compile lands instead of parsing the same script twice; that
    // compile started earlier, so waiting never takes longer than compiling.
    // Without `wait` (the precompiler itself) a held claim reports busy.
    Claim lookup_or_claim(const std::string& source, std::vector<std::uint8_t>& out,
                          bool wait) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (const auto* entry = find_locked(source)) {
                // Only an evaluation reading the bytecode is a hit; the
                // precompiler finding its work done is not.
                if (wait) {
                    out = entry->bytecode;
                    ++stats_.hits;
                }
                return Claim::hit;
            }
            if (!pending_locked(source)) {
                pending_.push_back(&source);
                return Claim::claimed;
            }
            if (!wait) return Claim::busy;
            ++stats_.waits;
            // Bounded: a claim holder that never finishes (it cannot; finish
            // and abandon are RAII-guarded) must not wedge the UI thread.
            if (!changed_.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return !pending_locked(source); })) {
                return Claim::busy;
            }
        }
    }

    void finish(const std::string& source, const std::uint8_t* bytes, std::size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_locked(source);
            if (bytes != nullptr && find_locked(source) == nullptr) {
                entries_.push_back({source, std::vector<std::uint8_t>(bytes, bytes + size)});
                stats_.bytes += source.size() + size;
                ++stats_.compiles;
                while (!entries_.empty()
                       && (entries_.size() > kMaxCachedScripts
                           || stats_.bytes > kMaxCachedBytes)) {
                    stats_.bytes -=
                        entries_.front().source.size() + entries_.front().bytecode.size();
                    entries_.pop_front();
                }
            }
        }
        changed_.notify_all();
    }

    void abandon(const std::string& source) { finish(source, nullptr, 0); }

    bool contains(const std::string& source) {
        std::lock_guard<std::mutex> lock(mutex_);
        return find_locked(source) != nullptr;
    }

    void note_precompiled() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.precompiled;
    }

    void note_bypass() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.bypassed;
    }

    ScriptBytecodeCacheStats stats() {
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
        std::string source;
        std::vector<std::uint8_t> bytecode;
    };

    const Entry* find_locked(const std::string& source) const {
        for (const auto& entry : entries_)
            if (entry.source.size() == source.size() && entry.source == source)
                return &entry;
        return nullptr;
    }

    // A claim is the claimer's own string, alive until it finishes.
    bool pending_locked(const std::string& source) const {
        for (const auto* claimed : pending_)
            if (claimed->size() == source.size() && *claimed == source) return true;
        return false;
    }

    void release_locked(const std::string& source) {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (*it == &source) {
                pending_.erase(it);
                return;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Entry> entries_;
    std::vector<const std::string*> pending_;
    ScriptBytecodeCacheStats stats_;
};

// Releases a claim on every exit path, so a compile that throws or fails can
// never leave a waiter blocked.
class ClaimGuard {
public:
    ClaimGuard(ScriptBytecodeCache& cache, const std::string& source)
        : cache_(cache), source_(source) {}
    ~ClaimGuard() {
        if (!done_) cache_.abandon(source_);
    }
    void finish(const std::uint8_t* bytes, std::size_t size) {
        done_ = true;
        cache_.finish(source_, bytes, size);
    }
    ClaimGuard(const ClaimGuard&) = delete;
    ClaimGuard& operator=(const ClaimGuard&) = delete;

private:
    ScriptBytecodeCache& cache_;
    const std::string& source_;
    bool done_ = false;
};

// Compiles global code to bytecode in `ctx` and serializes it; returns the
// exception value (caller owns it) when the source does not compile.
qjs::JSValue compile_global_script(qjs::JSContext* ctx, const std::string& code) {
    return qjs::JS_Eval(ctx, code.c_str(), code.size(), "",
                        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
}

} // namespace

ScriptBytecodeCacheStats script_bytecode_cache_stats() {
    return ScriptBytecodeCache::instance().stats();
}

void clear_script_bytecode_cache() {
    ScriptBytecodeCache::instance().clear();
}

namespace detail {
namespace {
std::mutex& precompile_hook_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::function<void(const std::string&)>& precompile_hook() {
    static std::function<void(const std::string&)> hook;
    return hook;
}
}  // namespace

void set_precompile_claim_hook_for_tests(std::function<void(const std::string&)> hook) {
    std::lock_guard<std::mutex> lock(precompile_hook_mutex());
    precompile_hook() = std::move(hook);
}

std::function<void(const std::string&)> precompile_claim_hook_for_tests() {
    std::lock_guard<std::mutex> lock(precompile_hook_mutex());
    return precompile_hook();
}
}  // namespace detail

std::size_t precompile_scripts(const std::vector<std::string>& sources,
                               const std::atomic<bool>* cancel) {
    auto& cache = ScriptBytecodeCache::instance();
    if (!ScriptBytecodeCache::enabled()) return 0;
    // A private runtime on the calling thread: it shares nothing with any
    // editor realm, runs no script, and is freed before returning. Bytecode is
    // runtime-independent (atoms serialize as strings), which is what lets an
    // editor realm read what this one wrote.
    std::optional<qjs::QuickJSContext> backend;
    std::size_t compiled = 0;
    for (const auto& code : sources) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        if (code.size() < kMinCacheableScriptBytes) continue;
        std::vector<std::uint8_t> unused;
        if (cache.lookup_or_claim(code, unused, /*wait=*/false)
                != ScriptBytecodeCache::Claim::claimed) {
            continue;  // already compiled, or another thread is compiling it
        }
        ClaimGuard guard(cache, code);
        if (auto hook = detail::precompile_claim_hook_for_tests())
            hook(code);  // holding the claim, before compiling
        if (!backend) {
            backend.emplace();
            set_quickjs_stack_size(*backend, 1024 * 1024);
        }
        auto* ctx = backend->context;
        if (!ctx) break;
        PULP_TRACE_SCOPE_NAMED("js", "script_precompile");
        auto function = compile_global_script(ctx, code);
        if (qjs::JS_IsException(function)) {
            // The editor's own evaluation reports the syntax error.
            qjs::JS_FreeValue(ctx, qjs::JS_GetException(ctx));
            continue;
        }
        std::size_t size = 0;
        auto* bytes = qjs::JS_WriteObject(ctx, &size, function, qjs::JS_WRITE_OBJ_BYTECODE);
        qjs::JS_FreeValue(ctx, function);
        if (!bytes) continue;
        guard.finish(bytes, size);
        qjs::js_free(ctx, bytes);
        cache.note_precompiled();
        ++compiled;
    }
    return compiled;
}

bool script_bytecode_cached(const std::string& source) {
    return ScriptBytecodeCache::instance().contains(source);
}

class QuickJsEngine final : public JsEngine {
public:
    QuickJsEngine() {
        auto backend = std::make_unique<qjs::QuickJSContext>();
        backend_ = backend.get();
        context_ = choc::javascript::Context(std::move(backend));
        set_quickjs_stack_size(*backend_, 1024 * 1024);  // 1MB (up from 256KB)
        // Surface unhandled promise failures through the host's console path.
        install_quickjs_rejection_tracker(*backend_);
        setup_console();
    }

    JsEngineType type() const override { return JsEngineType::quickjs; }
    bool is_valid() const override { return static_cast<bool>(context_); }

    choc::value::Value evaluate(const std::string& code) override {
        return context_.evaluateExpression(code);
    }

    // A large script compiles once per process: the bytecode of the first
    // compile is kept, and a later realm evaluating byte-identical source
    // deserializes it instead of parsing. Compile + execute is exactly what
    // JS_Eval does for global code, so results and exceptions match
    // evaluate(); any failure to reuse falls back to compiling.
    choc::value::Value evaluate_script(const std::string& code) override {
        auto& cache = ScriptBytecodeCache::instance();
        if (code.size() < kMinCacheableScriptBytes || !ScriptBytecodeCache::enabled()
            || !backend_ || !backend_->context) {
            if (code.size() >= kMinCacheableScriptBytes) cache.note_bypass();
            return evaluate(code);
        }
        using qjs::JS_TAG_UNDEFINED;  // JS_UNDEFINED expands to unqualified names
        using qjs::JSValue;
        auto* ctx = backend_->context;
        qjs::JSValue function = JS_UNDEFINED;
        std::vector<std::uint8_t> bytecode;
        auto claim = ScriptBytecodeCache::Claim::busy;
        {
            PULP_TRACE_SCOPE_NAMED("js", "script_bytecode_lookup");
            claim = cache.lookup_or_claim(code, bytecode, /*wait=*/true);
        }
        if (claim == ScriptBytecodeCache::Claim::hit) {
            PULP_TRACE_SCOPE_NAMED("js", "script_bytecode_read");
            function = qjs::JS_ReadObject(ctx, bytecode.data(), bytecode.size(),
                                          qjs::JS_READ_OBJ_BYTECODE);
            if (qjs::JS_IsException(function)) {
                qjs::JS_FreeValue(ctx, qjs::JS_GetException(ctx));
                function = JS_UNDEFINED;
            }
        }
        if (qjs::JS_IsUndefined(function)) {
            std::optional<ClaimGuard> guard;
            if (claim == ScriptBytecodeCache::Claim::claimed) guard.emplace(cache, code);
            PULP_TRACE_SCOPE_NAMED("js", "script_compile");
            function = compile_global_script(ctx, code);
            if (qjs::JS_IsException(function)) {
                // Surfaces the syntax error exactly as evaluate() would.
                return backend_->takeValue(function).toChocValue();
            }
            std::size_t size = 0;
            auto* bytes = qjs::JS_WriteObject(ctx, &size, function,
                                              qjs::JS_WRITE_OBJ_BYTECODE);
            if (guard) guard->finish(bytes, size);
            if (bytes) qjs::js_free(ctx, bytes);
        }
        PULP_TRACE_SCOPE_NAMED("js", "script_execute");
        // JS_EvalFunction takes ownership of the compiled function.
        return backend_->takeValue(qjs::JS_EvalFunction(ctx, function)).toChocValue();
    }

    bool supports_bounded_json_evaluation() const override { return true; }

    std::string evaluate_bounded_json(const std::string& code,
                                      std::size_t max_bytes) override {
        if (!backend_ || !backend_->context)
            throw std::runtime_error("QuickJS context is unavailable");
        auto value = backend_->takeValue(qjs::JS_Eval(
            backend_->context, code.data(), code.size(), "",
            0));
        value.throwIfError();
        return BoundedJsonWriter(*backend_, max_bytes).write(value.get());
    }

    void run_module(const std::string& code,
                    ModuleResolver resolver,
                    ModuleCompletionHandler completion) override {
        context_.runModule(code,
            [resolver = std::move(resolver)](std::string_view path) -> std::optional<std::string> {
                return resolver(path);
            },
            [completion = std::move(completion)](const std::string& error,
                                                 const choc::value::ValueView& result) mutable {
                if (completion) {
                    completion(error, choc::value::Value(result));
                }
            });
    }

    void register_function_impl(const std::string& name, NativeFunction fn) override {
        // Adapt Pulp's NativeFunction to CHOC's NativeFunction signature
        context_.registerFunction(name,
            [fn = std::move(fn)](choc::javascript::ArgumentList args) -> choc::value::Value {
                return fn(args.args, args.numArgs);
            });
    }

    choc::value::Value invoke(std::string_view name) override {
        return context_.invoke(name);
    }

    choc::value::Value invoke(std::string_view name,
                              const choc::value::Value* args,
                              size_t num_args) override {
        // CHOC's invoke is variadic template — we need to dispatch by arg count
        switch (num_args) {
            case 0: return context_.invoke(name);
            case 1: return context_.invoke(name, args[0]);
            case 2: return context_.invoke(name, args[0], args[1]);
            case 3: return context_.invoke(name, args[0], args[1], args[2]);
            case 4: return context_.invoke(name, args[0], args[1], args[2], args[3]);
            default: {
                // For 5+ args, build a JS call expression
                std::string call(name);
                call += '(';
                for (size_t i = 0; i < num_args; ++i) {
                    if (i > 0) call += ',';
                    call += choc::json::toString(args[i]);
                }
                call += ')';
                return context_.evaluateExpression(call);
            }
        }
    }

    void set_log_callback(LogCallback callback) override {
        log_callback_ = std::move(callback);
        setup_console();
    }

    void gc_hint() override {
        // QuickJS runs GC automatically; we can trigger a cycle via evaluate
        // but it's not necessary — the runtime handles it well.
    }

    void pump_message_loop() override {
        // CHOC's pumpMessageLoop is a no-op for QuickJS (#746); drive the
        // pending-job queue ourselves so queueMicrotask / Promise.then /
        // async-await actually drain when callers ask.
        pump_quickjs_jobs(*backend_);
    }

    bool supports_host_objects() const override { return true; }
    bool supports_promises() const override { return true; }

    // CHOC's QuickJS context installs a host interrupt handler backed by an
    // atomic `shouldCancel` flag and exposes it as Context::cancel(). Reuse it
    // rather than installing our own JS_SetInterruptHandler (which would clobber
    // CHOC's). cancel() is a thread-safe atomic store, so it is the sanctioned
    // way to abort a runaway evaluation from another thread; the next interrupt
    // check on the engine thread throws an "interrupted" exception. Returns
    // whether the engine reports interruption support; we don't gate on it here
    // because supports_interrupt() already advertises the capability.
    bool supports_interrupt() const override { return true; }
    void request_interrupt() override { context_.cancel(); }
    bool clear_pending_interrupt() override {
        return backend_->shouldCancel.exchange(false, std::memory_order_acq_rel);
    }

    // Expose the underlying CHOC context for WidgetBridge backward compatibility
    choc::javascript::Context& choc_context() { return context_; }

private:
    qjs::QuickJSContext* backend_ = nullptr;  // owned by context_
    choc::javascript::Context context_;
    LogCallback log_callback_;

    void setup_console() {
        choc::javascript::registerConsoleFunctions(context_,
            [this](std::string_view message, choc::javascript::LoggingLevel level) {
                if (log_callback_)
                    log_callback_(logging_level_name(level), message);
            });
    }
};

std::unique_ptr<JsEngine> create_quickjs_engine() {
    return std::make_unique<QuickJsEngine>();
}

} // namespace pulp::view
