#pragma once

// The delay primitive every recursive stage in the tank is built from.
//
// signal::DelayLineT already exists and is the right block for ordinary delay
// work, but the tank needs two things it does not offer: a 4-point Hermite read
// (linear interpolation zippers audibly on the slow, continuous read-position
// sweeps this reverb runs on every one of its 16 lines) and an index step with
// no integer modulo, because the inner loop performs on the order of 300 reads
// per sample. So this is a power-of-two-masked ring with both read paths.
//
// Block access. The tank processes up to one control period at a time, and
// every read in that span lands on a sample written BEFORE the span began
// (the shortest read is longer than the span — the tank enforces that bound).
// So a block reads with read_at() against a virtual write position `ahead`
// samples past the real one, and commits its writes afterwards with
// push_block(): the result is sample-for-sample what push/read interleaved
// would have produced, minus the per-sample bookkeeping.
//
// RT contract: prepare() allocates; push/read/reset allocate nothing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

#include <pulp/signal/detail/simd_lanes.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/interp.hpp>

namespace pulp::signal::fdn {

template <typename SampleType = float>
class FracDelayT {
  public:
    // Rounds up to a power of two with room for the Hermite window, so a read
    // at the maximum delay can still reach its two older neighbours.
    //
    // `stagger` offsets the ring inside its allocation by that many samples.
    // Owners of many same-sized rings that advance in lockstep (the tank's
    // lines, its diffusion stages) give each a different stagger: otherwise
    // every ring's write lands at the same low address bits as every other's,
    // and loads stall behind stores to unrelated rings.
    void prepare(int max_delay_samples, std::size_t stagger = 0) {
        const int needed = std::max(max_delay_samples, 1) + 4;
        std::size_t size = 8;
        while (size < static_cast<std::size_t>(needed)) size <<= 1;
        buffer_.assign(size + stagger + kMirror, SampleType{0});
        base_ = stagger;
        size_ = size;
        mask_ = size - 1;
        write_ = 0;
        max_delay_ = static_cast<int>(size) - 4;
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{0});
        write_ = 0;
    }

    int max_delay() const { return max_delay_; }

    void push(SampleType x) {
        SampleType* r = ring();
        r[write_] = x;
        // The mirror repeats the ring's first kMirror samples; a ring shorter
        // than the mirror appears in it more than once.
        for (std::size_t j = size_ + write_; j < size_ + kMirror; j += size_)
            r[j] = x;
        write_ = (write_ + 1) & mask_;
    }

    // Sample `delay` samples old (1 = the sample pushed most recently).
    SampleType read(int delay) const {
        const std::size_t idx = (write_ - static_cast<std::size_t>(delay)) & mask_;
        return ring()[idx];
    }

    // Integer read relative to a write position `ahead` samples past the
    // current one — the position the ring will have reached `ahead` pushes
    // from now. Only meaningful for delays longer than `ahead`.
    SampleType read_at(int ahead, int delay) const {
        const std::size_t idx =
            (write_ + static_cast<std::size_t>(ahead) - static_cast<std::size_t>(delay)) & mask_;
        return ring()[idx];
    }

    // Copy `n` consecutive samples, oldest first, starting `delay` samples
    // behind a write position `ahead` samples past the current one. A span
    // that does not cross the ring boundary is a plain contiguous copy.
    void copy_out(int ahead, int delay, SampleType* dst, int n) const {
        const std::size_t start =
            (write_ + static_cast<std::size_t>(ahead) - static_cast<std::size_t>(delay)) & mask_;
        const SampleType* src = ring();
        if (start + static_cast<std::size_t>(n) <= size_) {
            for (int k = 0; k < n; ++k)
                dst[k] = src[start + static_cast<std::size_t>(k)];
        } else {
            for (int k = 0; k < n; ++k)
                dst[k] = src[(start + static_cast<std::size_t>(k)) & mask_];
        }
    }

    // Commit `n` samples, oldest first: identical to n calls to push().
    void push_block(const SampleType* x, int n) {
        for (int done = 0; done < n; done += kMaxBlockSamples) {
            const int piece = std::min(kMaxBlockSamples, n - done);
            SampleType* dst = ring() + write_;
            for (int k = 0; k < piece; ++k)
                dst[k] = x[done + k];
            advance(piece);
        }
    }

    // As push_block(), reading every `stride`-th element of `x`.
    void push_strided(const SampleType* x, std::size_t stride, int n) {
        for (int k = 0; k < n; ++k)
            push(x[static_cast<std::size_t>(k) * stride]);
    }

    // Up to kMaxBlockSamples + 4 consecutive samples starting at ring index
    // `start` (masked), with no wrap: the ring keeps a copy of its first
    // kMirror samples just past its end. A Hermite read's four taps are one
    // 16-byte load from here, and a block's span is one contiguous run.
    const SampleType* taps_at(std::size_t start) const {
        return ring() + (start & mask_);
    }

    // Single-precision Hermite read against a position `ahead` pushes in the
    // future. Same taps and curve as read(double), evaluated in float: the
    // tank's block kernels run sixteen of these per sample, and a float
    // evaluation of a float ring is exact to the ring's own precision.
    SampleType read_hermite_at(int ahead, double delay) const {
        const double d = std::clamp(delay, 2.0, static_cast<double>(max_delay_));
        const int i = static_cast<int>(d);
        const auto f = static_cast<SampleType>(d - static_cast<double>(i));
        const std::size_t base =
            (write_ + static_cast<std::size_t>(ahead) - static_cast<std::size_t>(i));
        const SampleType* r = ring();
        return hermite4_t<SampleType>(r[(base - 2) & mask_], r[(base - 1) & mask_], r[base & mask_],
                                      r[(base + 1) & mask_], SampleType{1} - f);
    }

    // n Hermite reads in float, sample k at delays[k] behind a write position
    // k - behind pushes ahead, as read_hermite_at(k - behind, delays[k])
    // would give. `behind` lets a caller that has already pushed the span read
    // each sample against its own post-push position (behind = n - 1) without
    // folding the offset into the fractional delay, whose rounding would then
    // depend on the block length. When every read in the span shares one
    // integer delay and its taps do not cross the ring boundary, consecutive
    // samples' taps are consecutive in memory and the span interpolates in one
    // vectorizable pass; otherwise each read gathers its own taps.
    // n <= kMaxBlockSamples.
    void read_hermite_span(const double* delays, SampleType* out, int n, int behind = 0) const {
        const double whole = std::floor(delays[0]);
        if (whole >= 2.0 && whole + 1.0 <= static_cast<double>(max_delay_)) {
            const std::size_t start =
                (write_ - static_cast<std::size_t>(behind) - static_cast<std::size_t>(whole) - 2) &
                mask_;
            SampleType t[kMaxBlockSamples];
            // Every read shares the integer delay exactly when every fraction
            // relative to it lands in [0, 1) — checked on the fractions the
            // reads would use, so the fast path never disagrees with
            // read_hermite_at().
            if (start + static_cast<std::size_t>(n) + 3 <= size_ + kMirror &&
                fractions(delays, whole, t, n)) {
                hermite_run(ring() + start, t, out, n);
                return;
            }
        }
        for (int k = 0; k < n; ++k)
            out[k] = read_hermite_at(k - behind, delays[k]);
    }

    // read_hermite_span() with one delay for the whole span.
    void read_hermite_span(double delay, SampleType* out, int n, int behind = 0) const {
        const double d = std::clamp(delay, 2.0, static_cast<double>(max_delay_));
        const double whole = std::floor(d);
        const std::size_t start =
            (write_ - static_cast<std::size_t>(behind) - static_cast<std::size_t>(whole) - 2) &
            mask_;
        if (start + static_cast<std::size_t>(n) + 3 <= size_ + kMirror) {
            SampleType t[kMaxBlockSamples];
            const SampleType tk = SampleType{1} - static_cast<SampleType>(d - whole);
            for (int k = 0; k < n; ++k)
                t[k] = tk;
            hermite_run(ring() + start, t, out, n);
            return;
        }
        for (int k = 0; k < n; ++k)
            out[k] = read_hermite_at(k - behind, delay);
    }

    // Raw ring access for block kernels that address the buffer themselves.
    SampleType* data() {
        return ring();
    }
    std::size_t size() const {
        return size_;
    }
    std::size_t mask() const {
        return mask_;
    }
    std::size_t write_index() const {
        return write_;
    }
    // Samples mirrored past the ring's end (see taps_at()).
    static constexpr std::size_t mirror_samples() {
        return kMirror;
    }

    // advance() for a run the caller knows touched neither the ring's first
    // mirror_samples() samples nor its end: no mirror upkeep is due.
    void skip(int n) {
        write_ = (write_ + static_cast<std::size_t>(n)) & mask_;
    }

    // Advance the write position past `n` samples the caller stored directly
    // as one contiguous run from data() + write_index() (which may extend
    // into the mirror past the ring's end, n <= kMaxBlockSamples). Moves any
    // part written past the end to its ring index, newest last so it wins
    // when the run is longer than the ring, then refreshes the mirror if the
    // ring's first samples changed.
    void advance(int n) {
        const std::size_t end = write_ + static_cast<std::size_t>(n);
        SampleType* r = ring();
        if (end > size_)
            for (std::size_t j = size_; j < end; ++j)
                r[j & mask_] = r[j];
        if (write_ < kMirror || end > size_)
            refresh_mirror();
        write_ = end & mask_;
    }

    // Fractional read. `delay` is clamped into the window where all four
    // Hermite taps exist, so an out-of-range modulation excursion degrades to
    // the nearest legal read instead of walking off the allocation.
    SampleType read(double delay) const {
        const double d = std::clamp(delay, 2.0, static_cast<double>(max_delay_));
        const int i = static_cast<int>(d);
        const double f = d - static_cast<double>(i);
        // Older samples sit at larger delays, so the time-ordered Hermite taps
        // run from delay i+2 (oldest) down to i-1 (newest); the fractional
        // position runs the other way, hence 1 - f.
        return static_cast<SampleType>(hermite4(
            static_cast<double>(read(i + 2)), static_cast<double>(read(i + 1)),
            static_cast<double>(read(i)), static_cast<double>(read(i - 1)), 1.0 - f));
    }

  private:
    // Samples copied past the ring's end so a block's span, or its taps,
    // never wrap.
    static constexpr std::size_t kMirror = static_cast<std::size_t>(kMaxBlockSamples) + 4;

    void refresh_mirror() {
        if (size_ == 0)
            return;
        SampleType* r = ring();
        for (std::size_t j = 0; j < kMirror; ++j)
            r[size_ + j] = r[j];
    }

    SampleType* ring() {
        return buffer_.data() + base_;
    }
    const SampleType* ring() const {
        return buffer_.data() + base_;
    }

    // t[k] = 1 - (delays[k] - whole) in SampleType; false when any fraction
    // falls outside [0, 1), i.e. a read does not share the integer delay.
    static bool fractions(const double* delays, double whole, SampleType* t, int n) {
        namespace lanes = detail::lanes;
        int k = 0;
        bool inside = true;
        if constexpr (std::is_same_v<SampleType, float>) {
            const lanes::D2 base = lanes::splat(whole);
            const lanes::F4 zero = lanes::splat(0.0f);
            const lanes::F4 one = lanes::splat(1.0f);
            auto ok = lanes::less_equal(zero, zero);
            for (; k + 4 <= n; k += 4) {
                const lanes::F4 f = lanes::narrow(lanes::load(delays + k) - base,
                                                  lanes::load(delays + k + 2) - base);
                ok = lanes::both(ok, lanes::both(lanes::less_equal(zero, f), lanes::less(f, one)));
                lanes::store(t + k, one - f);
            }
            inside = lanes::all(ok);
        }
        for (; k < n; ++k) {
            const auto f = static_cast<SampleType>(delays[k] - whole);
            inside = inside && f >= SampleType{0} && f < SampleType{1};
            t[k] = SampleType{1} - f;
        }
        return inside;
    }

    // out[k] = Hermite of p[k .. k + 3] at t[k]: the contiguous-taps case.
    // Four samples at a time as explicit vectors (the expression is
    // Interpolator::hermite's, term for term), then a scalar tail.
    static void hermite_run(const SampleType* p, const SampleType* t, SampleType* out, int n) {
        int k = 0;
        if constexpr (std::is_same_v<SampleType, float>) {
            namespace lanes = detail::lanes;
            const lanes::F4 half = lanes::splat(0.5f);
            const lanes::F4 two_and_half = lanes::splat(2.5f);
            const lanes::F4 two = lanes::splat(2.0f);
            const lanes::F4 one_and_half = lanes::splat(1.5f);
            for (; k + 4 <= n; k += 4) {
                const lanes::F4 ym1 = lanes::load(p + k);
                const lanes::F4 y0 = lanes::load(p + k + 1);
                const lanes::F4 y1 = lanes::load(p + k + 2);
                const lanes::F4 y2 = lanes::load(p + k + 3);
                const lanes::F4 frac = lanes::load(t + k);
                const lanes::F4 c0 = y0;
                const lanes::F4 c1 = half * (y1 - ym1);
                const lanes::F4 c2 = ym1 - two_and_half * y0 + two * y1 - half * y2;
                const lanes::F4 c3 = half * (y2 - ym1) + one_and_half * (y0 - y1);
                lanes::store(out + k, ((c3 * frac + c2) * frac + c1) * frac + c0);
            }
        }
        for (; k < n; ++k)
            out[k] = hermite4_t<SampleType>(p[k], p[k + 1], p[k + 2], p[k + 3], t[k]);
    }

    std::vector<SampleType> buffer_;
    std::size_t base_ = 0;
    std::size_t size_ = 0;
    std::size_t mask_ = 0;
    std::size_t write_ = 0;
    int max_delay_ = 0;
};

using FracDelay = FracDelayT<float>;

}  // namespace pulp::signal::fdn
