#pragma once

// RetireList — a grace-period reclamation list for engine stacks the audio thread
// swaps between via published atomic pointers.
//
// The processor publishes a fresh engine (CPU oracle / GPU stack / IR convolver)
// through an atomic pointer and must free the previous one WITHOUT the audio
// thread ever freeing or using-after-free. A plain "retire one, free at the next
// rebuild" scheme is unsafe: two rebuilds can complete within a single (large-
// buffer) audio callback, freeing an engine the callback still holds.
//
// Instead, the audio thread bumps a monotonic epoch at the top of every process()
// call. When the worker retires an engine it records the current epoch; the engine
// is freed only once the epoch has advanced by ``kGrace``. Because audio callbacks
// are serial (one at a time), an epoch advance of +1 proves the callback that
// might have loaded the old pointer has returned; +kGrace (2) keeps a conservative
// margin. Objects are held type-erased via ``shared_ptr<void>`` so one list serves
// every engine type; the shared_ptr's stored deleter runs the correct destructor.
//
// Threading: the list is touched only off the audio thread (the worker while it
// runs; prepare()/release() while the worker is stopped) — never concurrently. The
// audio thread only bumps the epoch it reads. No lock is needed on the list.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::examples::nam {

class RetireList {
public:
    // Epochs the audio thread must advance past a retirement before the retired
    // object is freed. +1 is provably sufficient (serial callbacks); +2 is margin.
    static constexpr std::uint64_t kGrace = 2;

    // Record an engine for grace-period reclamation. Pass the epoch read AFTER the
    // old pointer was unpublished, so any callback that could hold it has epoch
    // <= that value.
    void retire(std::shared_ptr<void> obj, std::uint64_t retire_epoch) {
        if (obj) items_.push_back({std::move(obj), retire_epoch});
    }

    // Free every retiree the audio thread has provably cycled past.
    void reclaim(std::uint64_t audio_epoch) {
        items_.erase(
            std::remove_if(items_.begin(), items_.end(),
                           [&](const Item& it) { return audio_epoch >= it.epoch + kGrace; }),
            items_.end());
    }

    // Free everything now — only safe when the audio thread is stopped
    // (prepare()/release()).
    void clear() { items_.clear(); }

    std::size_t size() const { return items_.size(); }

private:
    struct Item {
        std::shared_ptr<void> obj;
        std::uint64_t epoch;
    };
    std::vector<Item> items_;
};

}  // namespace pulp::examples::nam
