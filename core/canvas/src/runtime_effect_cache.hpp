#pragma once

// RuntimeEffectCache — compile-and-cache SkRuntimeEffect shaders by source hash.
// Lives at process lifetime (static), NOT on SkiaCanvas (which is recreated every frame).

#ifdef PULP_HAS_SKIA

#include "include/effects/SkRuntimeEffect.h"
#include <string>
#include <unordered_map>
#include <mutex>

namespace pulp::canvas {

class RuntimeEffectCache {
public:
    static RuntimeEffectCache& instance() {
        static RuntimeEffectCache cache;
        return cache;
    }

    // Compile and cache an SkSL shader. Returns nullptr on failure.
    // Thread-safe via mutex (one compilation at a time).
    sk_sp<SkRuntimeEffect> get_or_compile(const std::string& sksl) {
        std::string ignored;
        return get_or_compile(sksl, ignored);
    }

    // Compile and cache an SkSL shader, returning the error text for THIS
    // compile via `error_out` (empty on success). The error is a local of this
    // call, never a cache-global: a previous version stored it in a shared
    // `last_error_` member written between two lock scopes, so a concurrent
    // compile of a different shader could overwrite it and a caller reading it
    // afterwards would attribute another shader's error to its own. Returning
    // the error from the call removes that race entirely.
    sk_sp<SkRuntimeEffect> get_or_compile(const std::string& sksl,
                                          std::string& error_out) {
        error_out.clear();
        auto hash = std::hash<std::string>{}(sksl);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = effects_.find(hash);
            if (it != effects_.end()) return it->second;
        }

        // Compile outside lock (may be slow).
        auto result = SkRuntimeEffect::MakeForShader(SkString(sksl.c_str()));
        if (!result.effect) {
            error_out = result.errorText.c_str();
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        effects_[hash] = result.effect;
        return result.effect;
    }

    // Clear all cached effects (for hot reload)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        effects_.clear();
    }

private:
    RuntimeEffectCache() = default;
    std::unordered_map<size_t, sk_sp<SkRuntimeEffect>> effects_;
    std::mutex mutex_;
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
