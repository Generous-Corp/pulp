#pragma once

/// @file freeze_hold.hpp
/// Spectral freeze / infinite hold for phase-vocoder frame groups.
///
/// Captures a rolling window of recent analysis frames (preallocated at
/// prepare) and, when engaged, replaces live analysis frames with a
/// synthetic steady-state hold: per-channel magnitudes averaged over the
/// captured window, per-bin phases advancing at the instantaneous
/// frequency estimated from the captured frames, plus a small bounded
/// random walk that de-periodicizes the hold (spectral freezing per
/// J.-F. Charles, "A Tutorial on Spectral Sound Processing Using Max/MSP
/// and Jitter," CMJ 32(3), 2008; the random-walk de-looping follows the
/// granular-hold contrast family, Roads, *Microsound*, 2001).
///
/// Designed to sit at the HEAD of a vocoder chain (before phase
/// propagation and formant processing): held frames look like live
/// steady-state input, so downstream pitch/formant control keeps working
/// over frozen audio. Per-channel phase offsets are initialized from the
/// latched frame and every channel advances by the same per-bin amount,
/// preserving the spatial image. The per-bin advance is estimated from all
/// channels together (magnitude-weighted), so an anti-phase bin — whose
/// channel sum cancels — still gets its true frequency.
///
/// Engage policy (explicit no-mute rule): freezing latches only once the
/// capture window is full; until then live frames pass through and the
/// latch arms. The hold averages exactly the last `capture_frames`
/// consecutive analysis frames before the latch. A release is committed:
/// the old hold fades out even if a freeze is requested again during the
/// fade, and the capture window restarts at the release, so the next hold
/// contains only input analyzed after it. Engage/release crossfade over a
/// fixed number of frames in the spectral domain; `engage_progress()`
/// reports the fade position. Consumers that align their own (e.g.
/// time-domain) crossfade can instead render the hold directly through the
/// public pipeline: `write_hold()`, `advance_hold()`,
/// `rewind_hold_phases()`, and the held-state accessors.
///
/// Deterministic (xorshift PRNG seeded at prepare); no allocation or locks
/// after prepare(). All members except `stage_restore()` and
/// `restore_pending()` belong to the thread that runs process_group();
/// state such as is_latched() must reach a UI through the owner's own
/// publication, not by reading this object from another thread.
///
/// A held state can be captured with `snapshot()` into caller-prepared
/// storage, serialized to a versioned, size-bounded byte image, and
/// recalled with `stage_restore()` from a non-audio thread. The audio
/// owner adopts a staged restore at the start of the next frame group.

#include <pulp/signal/checked_allocation.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace pulp::signal {

/// How a restored hold enters the output.
enum class FreezeRestoreEngage : std::uint8_t {
    /// Fade in over the configured crossfade, like a normal engage.
    crossfade,
    /// Replace the output with the hold from the adopting frame onward.
    immediate,
    /// Resume the recorded request and fade position (a hold captured
    /// mid-release keeps releasing).
    as_captured,
};

/// Value image of a FreezeHold's held state: everything needed to reproduce
/// the held output bit-for-bit on a hold prepared with the same geometry.
///
/// Storage is allocated by `prepare()` (control thread). Byte
/// serialization allocates and is not audio-thread safe; `snapshot()` and
/// `stage_restore()` only copy into prepared storage.
///
/// Byte image (little-endian, format version 1), 56-byte header:
///   "PFHS" magic, u32 version, u32 fft_size, u32 channels,
///   u32 analysis_hop, u32 capture_frames, u32 crossfade_frames,
///   u32 fade_step, u32 flags (bit 0 engaged, bit 1 releasing),
///   u32 reserved (0), f64 sample_rate (0 = unknown), u64 rng_state
/// then f32 magnitudes[channels * bins], f64 phases[channels * bins],
/// f64 inst_freq[bins], where bins = fft_size / 2 + 1. Phases are wrapped
/// to [-pi, pi]; instantaneous frequencies are radians per sample. The
/// largest supported image (16384-point, 16 channels) is under 2 MB.
template <typename SampleType = float>
struct FreezeHoldSnapshotT {
    static constexpr std::uint32_t kFormatVersion = 1;
    static constexpr int kMinFftSize = 256;
    static constexpr int kMaxFftSize = 16384;
    static constexpr int kMaxChannels = 16;
    static constexpr std::size_t kHeaderBytes = 56;
    static constexpr std::uint32_t kFlagEngaged = 1u;
    static constexpr std::uint32_t kFlagReleasing = 2u;

    int fft_size = 0;
    int channels = 0;
    int analysis_hop = 0;
    /// Frames averaged into the hold (informational).
    int capture_frames = 0;
    /// Crossfade length and position when the snapshot was taken.
    int crossfade_frames = 1;
    int fade_step = 0;
    bool engaged = false;
    bool releasing = false;
    /// Sample rate of the capture, or 0 when the hold was not told one. A
    /// restore refuses a known rate that differs from the receiver's.
    double sample_rate = 0.0;
    std::uint64_t rng_state = 0;
    std::vector<SampleType> magnitudes; // channels * bins
    std::vector<double> phases;         // channels * bins, wrapped
    std::vector<double> inst_freq;      // bins, radians/sample

    static bool valid_geometry(int fft, int ch, int hop) noexcept {
        return fft >= kMinFftSize && fft <= kMaxFftSize && (fft & (fft - 1)) == 0
            && ch >= 1 && ch <= kMaxChannels && hop > 0 && hop <= fft / 2;
    }

    /// Byte size of the serialized image for a geometry, or 0 when the
    /// geometry is outside the supported bounds.
    static std::size_t serialized_size(int fft, int ch) noexcept {
        if (!valid_geometry(fft, ch, 1)) return 0;
        const auto bins = static_cast<std::size_t>(fft / 2 + 1);
        const auto channel_bins = static_cast<std::size_t>(ch) * bins;
        return kHeaderBytes + channel_bins * sizeof(float)
             + channel_bins * sizeof(double) + bins * sizeof(double);
    }

    /// Allocate storage for a geometry. Not audio-thread safe.
    bool prepare(int fft, int ch, int hop) {
        if (!valid_geometry(fft, ch, hop)) return false;
        const auto bins = static_cast<std::size_t>(fft / 2 + 1);
        fft_size = fft;
        channels = ch;
        analysis_hop = hop;
        capture_frames = 0;
        crossfade_frames = 1;
        fade_step = 0;
        engaged = false;
        releasing = false;
        sample_rate = 0.0;
        rng_state = 0;
        magnitudes.assign(static_cast<std::size_t>(ch) * bins, SampleType{0});
        phases.assign(static_cast<std::size_t>(ch) * bins, 0.0);
        inst_freq.assign(bins, 0.0);
        return true;
    }

    /// Shape and value check: geometry in bounds, storage sized for it,
    /// a consistent fade record, every value finite, magnitudes
    /// non-negative, phases wrapped, and a non-zero PRNG state (zero is a
    /// fixed point of xorshift).
    bool valid() const noexcept {
        if (!valid_geometry(fft_size, channels, analysis_hop) || capture_frames < 0
            || crossfade_frames < 1 || fade_step < 0 || fade_step > crossfade_frames
            || (engaged && releasing) || !std::isfinite(sample_rate)
            || sample_rate < 0.0 || rng_state == 0)
            return false;
        const auto bins = static_cast<std::size_t>(fft_size / 2 + 1);
        const auto channel_bins = static_cast<std::size_t>(channels) * bins;
        if (magnitudes.size() != channel_bins || phases.size() != channel_bins
            || inst_freq.size() != bins)
            return false;
        constexpr double kPhaseBound = 3.14159265358979323846 + 1e-6;
        for (const auto m : magnitudes)
            if (!std::isfinite(m) || m < SampleType{0}) return false;
        for (const auto p : phases)
            if (!std::isfinite(p) || std::abs(p) > kPhaseBound) return false;
        for (const auto f : inst_freq)
            if (!std::isfinite(f) || std::abs(f) > 2.0 * kPhaseBound) return false;
        return true;
    }

    /// Serialize to the versioned byte image. Allocates; not audio-thread
    /// safe. Returns false (and leaves `out` empty) for an invalid image.
    bool write_bytes(std::vector<std::uint8_t>& out) const {
        out.clear();
        if (!valid()) return false;
        out.reserve(serialized_size(fft_size, channels));
        const std::uint8_t magic[4] = {'P', 'F', 'H', 'S'};
        out.insert(out.end(), magic, magic + 4);
        put_u32_(out, kFormatVersion);
        put_u32_(out, static_cast<std::uint32_t>(fft_size));
        put_u32_(out, static_cast<std::uint32_t>(channels));
        put_u32_(out, static_cast<std::uint32_t>(analysis_hop));
        put_u32_(out, static_cast<std::uint32_t>(capture_frames));
        put_u32_(out, static_cast<std::uint32_t>(crossfade_frames));
        put_u32_(out, static_cast<std::uint32_t>(fade_step));
        put_u32_(out, (engaged ? kFlagEngaged : 0u) | (releasing ? kFlagReleasing : 0u));
        put_u32_(out, 0u);
        put_f64_(out, sample_rate);
        put_u64_(out, rng_state);
        for (const auto m : magnitudes) {
            const auto f = static_cast<float>(m);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            put_u32_(out, bits);
        }
        for (const auto p : phases) put_f64_(out, p);
        for (const auto f : inst_freq) put_f64_(out, f);
        return true;
    }

    /// Parse a byte image. Rejects a wrong magic or version, an
    /// out-of-bounds geometry, a size that does not match the geometry
    /// exactly, unknown flag or reserved bits, and any invalid value.
    /// Allocates; not audio-thread safe. `out` is only modified on success.
    static bool read_bytes(const std::uint8_t* data, std::size_t size,
                           FreezeHoldSnapshotT& out) {
        if (data == nullptr || size < kHeaderBytes
            || std::memcmp(data, "PFHS", 4) != 0)
            return false;
        std::size_t at = 4;
        if (get_u32_(data, at) != kFormatVersion) return false;
        const auto fft = get_u32_(data, at);
        const auto ch = get_u32_(data, at);
        const auto hop = get_u32_(data, at);
        const auto depth = get_u32_(data, at);
        const auto crossfade = get_u32_(data, at);
        const auto step = get_u32_(data, at);
        const auto flags = get_u32_(data, at);
        const auto reserved = get_u32_(data, at);
        constexpr std::uint32_t kIntBound = 0x7fffffffu;
        if (fft > static_cast<std::uint32_t>(kMaxFftSize)
            || ch > static_cast<std::uint32_t>(kMaxChannels)
            || hop > static_cast<std::uint32_t>(kMaxFftSize) || depth > kIntBound
            || crossfade > kIntBound || step > kIntBound
            || (flags & ~(kFlagEngaged | kFlagReleasing)) != 0 || reserved != 0)
            return false;
        FreezeHoldSnapshotT image;
        if (!image.prepare(static_cast<int>(fft), static_cast<int>(ch),
                           static_cast<int>(hop))
            || size != serialized_size(image.fft_size, image.channels))
            return false;
        image.capture_frames = static_cast<int>(depth);
        image.crossfade_frames = static_cast<int>(crossfade);
        image.fade_step = static_cast<int>(step);
        image.engaged = (flags & kFlagEngaged) != 0;
        image.releasing = (flags & kFlagReleasing) != 0;
        image.sample_rate = get_f64_(data, at);
        image.rng_state = get_u64_(data, at);
        for (auto& m : image.magnitudes) {
            const std::uint32_t bits = get_u32_(data, at);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            m = static_cast<SampleType>(f);
        }
        for (auto& p : image.phases) p = get_f64_(data, at);
        for (auto& f : image.inst_freq) f = get_f64_(data, at);
        if (at != size || !image.valid()) return false;
        out = std::move(image);
        return true;
    }

private:
    static void put_u32_(std::vector<std::uint8_t>& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
    static void put_u64_(std::vector<std::uint8_t>& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
    static void put_f64_(std::vector<std::uint8_t>& out, double v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        put_u64_(out, bits);
    }
    static std::uint32_t get_u32_(const std::uint8_t* data, std::size_t& at) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4;
        return v;
    }
    static std::uint64_t get_u64_(const std::uint8_t* data, std::size_t& at) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 8;
        return v;
    }
    static double get_f64_(const std::uint8_t* data, std::size_t& at) {
        const std::uint64_t bits = get_u64_(data, at);
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
};

using FreezeHoldSnapshot = FreezeHoldSnapshotT<float>;
using FreezeHoldSnapshot64 = FreezeHoldSnapshotT<double>;

/// Time-based FreezeHold timing that reproduces the frame-count defaults
/// (8 capture frames, 6 crossfade frames, 0.015 rad/frame walk) at the
/// reference geometry they were tuned for: a 512-sample analysis hop at
/// 48 kHz (RealtimePitchTimeProcessor's `quality` mode, 4096/512).
struct FreezeHoldReferenceTiming {
    static constexpr double kSampleRate = 48000.0;
    static constexpr int kAnalysisHop = 512;
    /// 8 hops: 8 * 512 / 48000 s.
    static constexpr double kCaptureSeconds = 8.0 * kAnalysisHop / kSampleRate;
    /// 6 hops: 6 * 512 / 48000 s.
    static constexpr double kCrossfadeSeconds = 6.0 * kAnalysisHop / kSampleRate;
    /// 0.015 rad per 512-sample frame, as a random-walk rate:
    /// 0.015 * sqrt(48000 / 512) rad per sqrt(second).
    static constexpr double kPhaseJitterPerSqrtSecond = 0.14523687548277813;
};

template <typename SampleType = float>
class FreezeHoldT {
public:
    /// Two sizing modes. With `sample_rate == 0` (the default) the frame
    /// counts are used as given. With `sample_rate > 0` the seconds fields
    /// are used instead and the frame counts are derived from the hop, so
    /// the hold keeps the same real-time behaviour at any geometry; their
    /// defaults reproduce the frame-count defaults at a 512-sample hop and
    /// 48 kHz (see FreezeHoldReferenceTiming).
    struct Config {
        int fft_size = 2048;
        int channels = 1;
        int analysis_hop = 512;
        /// Frames averaged into the hold (rolling capture depth, >= 2).
        int capture_frames = 8;
        /// Engage/release crossfade length in frames (>= 1).
        int crossfade_frames = 6;
        /// Random-walk step bound (radians/frame) for de-looping.
        float phase_jitter = 0.015f;
        /// Largest capture depth set_capture_frames() may select later;
        /// storage is preallocated for it. 0 = capture_frames (no headroom).
        int max_capture_frames = 0;

        /// > 0 selects seconds-based sizing.
        double sample_rate = 0.0;
        /// Capture window in seconds of hop grid: frames = round(s * sr / hop).
        double capture_seconds = FreezeHoldReferenceTiming::kCaptureSeconds;
        /// Crossfade length: frames = round(s * sr / hop).
        double crossfade_seconds = FreezeHoldReferenceTiming::kCrossfadeSeconds;
        /// Random-walk rate in radians per sqrt(second); the per-frame bound
        /// is rate * sqrt(hop / sr), so the walk's spread over time does not
        /// depend on the hop.
        double phase_jitter_per_sqrt_second =
            FreezeHoldReferenceTiming::kPhaseJitterPerSqrtSecond;
        /// Longest capture set_capture_seconds() may select later.
        /// 0 = capture_seconds (no headroom).
        double max_capture_seconds = 0.0;
    };

    using Snapshot = FreezeHoldSnapshotT<SampleType>;

    /// Resolve the time-based fields of `config` into frame counts: the
    /// exact configuration prepare() runs with. Pure.
    static Config resolve(const Config& config) noexcept {
        Config out = config;
        if (config.sample_rate > 0.0 && config.analysis_hop > 0) {
            out.capture_frames = frames_for_seconds(config.capture_seconds, config);
            out.crossfade_frames = frames_for_seconds(config.crossfade_seconds, config);
            out.phase_jitter = static_cast<float>(
                std::max(config.phase_jitter_per_sqrt_second, 0.0)
                * std::sqrt(static_cast<double>(config.analysis_hop) / config.sample_rate));
            out.max_capture_frames = frames_for_seconds(config.max_capture_seconds, config);
        }
        out.capture_frames = std::max(out.capture_frames, 2);
        out.crossfade_frames = std::max(out.crossfade_frames, 1);
        out.max_capture_frames = std::max(out.max_capture_frames, out.capture_frames);
        return out;
    }

    /// round(seconds * sample_rate / analysis_hop) for a seconds-based
    /// config (0 when it is not seconds-based or `seconds` is not positive).
    static int frames_for_seconds(double seconds, const Config& config) noexcept {
        if (!(config.sample_rate > 0.0) || config.analysis_hop <= 0 || !(seconds > 0.0))
            return 0;
        const double frames =
            seconds * config.sample_rate / static_cast<double>(config.analysis_hop);
        return static_cast<int>(std::lround(std::min(frames, 1.0e6)));
    }

    static bool checked_retained_bytes(const Config& requested,
                                       std::uint64_t target_max_bytes,
                                       std::uint64_t& bytes) noexcept {
        const Config config = resolve(requested);
        const auto bins = static_cast<std::uint64_t>(config.fft_size / 2 + 1);
        const auto channels = static_cast<std::uint64_t>(config.channels);
        const auto depth = static_cast<std::uint64_t>(config.max_capture_frames);
        std::uint64_t channel_bins = 0;
        std::uint64_t channel_capture_bins = 0;
        if (!checked_capacity_product(channels, bins, UINT64_MAX, channel_bins)
            || !checked_capacity_product(depth, channel_bins, UINT64_MAX,
                                         channel_capture_bins))
            return false;
        CheckedRetainedByteCharge charge(target_max_bytes);
        // Capture magnitude ring, newest phase increment, previous captured
        // frame, the held state, and the equally sized staged-restore slot.
        if (!charge.add<SampleType>(channel_capture_bins)
            || !charge.add<double>(bins)
            || !charge.add<std::complex<SampleType>>(channel_bins)
            || !charge.add<SampleType>(channel_bins)
            || !charge.add<double>(channel_bins) || !charge.add<double>(bins)
            || !charge.add<SampleType>(channel_bins)
            || !charge.add<double>(channel_bins) || !charge.add<double>(bins))
            return false;
        bytes = charge.total();
        return true;
    }

    /// RT contract: prepare() allocates capture/hold storage and is not
    /// audio-thread safe. After prepare(), every other member is
    /// allocation-free for the prepared channel/bin counts. prepare()
    /// drops a staged restore.
    void prepare(const Config& requested) {
        const Config config = resolve(requested);
        assert(config.fft_size >= 256 && (config.fft_size & (config.fft_size - 1)) == 0);
        assert(config.channels >= 1);
        config_ = config;
        num_bins_ = config.fft_size / 2 + 1;

        const auto bins = static_cast<size_t>(num_bins_);
        const auto channels = static_cast<size_t>(config.channels);
        const auto depth = static_cast<size_t>(config.max_capture_frames);
        capture_mag_.assign(channels * depth * bins, SampleType{0});
        latest_increment_.assign(bins, 0.0);
        prev_frame_.assign(channels * bins, std::complex<SampleType>{});
        held_mag_.assign(channels * bins, SampleType{0});
        held_phase_.assign(channels * bins, 0.0);
        inst_freq_.assign(bins, 0.0);
        pending_mag_.assign(channels * bins, SampleType{0});
        pending_phase_.assign(channels * bins, 0.0);
        pending_inst_freq_.assign(bins, 0.0);
        pending_.value.store(kRestoreIdle, std::memory_order_release);
        active_capture_frames_ = config.capture_frames;
        latched_capture_frames_ = 0;
        reset();
    }

    /// The resolved configuration this hold was prepared with.
    const Config& config() const { return config_; }

    /// Hold length: the number of most recent frames the NEXT latch
    /// averages, clamped to [2, max_capture_frames]. RT-safe; it does not
    /// change a hold already playing. Survives reset() and clear_history().
    /// A shorter window freezes closer to "now"; a longer one blends more
    /// of the recent past into a smoother hold.
    void set_capture_frames(int frames) {
        if (num_bins_ == 0) return; // not prepared
        active_capture_frames_ = std::clamp(frames, 2, config_.max_capture_frames);
    }
    /// Seconds-based form of set_capture_frames(); ignored unless the hold
    /// was prepared with a sample rate.
    void set_capture_seconds(double seconds) {
        if (config_.sample_rate > 0.0)
            set_capture_frames(frames_for_seconds(seconds, config_));
    }
    /// The capture depth the next latch uses.
    int capture_frames() const { return active_capture_frames_; }

    /// Full reset: drops the hold and the capture history and re-seeds the
    /// PRNG (the prepared state). For a transport discontinuity that should
    /// keep an engaged hold, use clear_history() instead. A restore staged
    /// by stage_restore() stays pending and is adopted by the next
    /// process_group(), so a host that resets after recalling state still
    /// hears the recalled hold.
    void reset() {
        clear_history();
        engaged_ = false;
        latched_ = false;
        releasing_ = false;
        has_hold_ = false;
        fade_ = SampleType{0};
        fade_step_ = 0;
        rng_ = kRngSeed;
    }

    /// Forget captured input only. The hold, its request and fade position,
    /// and the PRNG are kept, so a playing hold continues seamlessly across
    /// a transport jump; an armed or later freeze re-fills its capture
    /// window from input analyzed after this call.
    void clear_history() {
        // Only the count matters: a latch reads the newest `captured_`
        // slots, all written since this call, so nothing needs zeroing.
        captured_ = 0;
        capture_pos_ = 0;
        prev_valid_ = false;
    }

    /// RT-safe engage/release request. Releasing a latched hold commits it
    /// to fade out and restarts the capture window, so the next freeze
    /// averages only frames analyzed from the release onward (re-arming,
    /// without muting, until the window is full again) — also when the
    /// freeze is requested again before the release fade has finished.
    void set_frozen(bool frozen) {
        if (engaged_ && !frozen && latched_) {
            captured_ = 0;
            releasing_ = true;
        }
        engaged_ = frozen;
    }
    /// The last engage/release request.
    bool is_engaged() const { return engaged_; }
    /// True once held content is actually playing (capture was full). Stays
    /// true through the release fade.
    bool is_latched() const { return latched_; }
    /// True while a released hold is fading out.
    bool is_releasing() const { return releasing_; }
    /// True once a hold has been latched or restored since the last reset();
    /// the held-state accessors and write_hold() describe it.
    bool has_hold() const { return has_hold_; }

    /// Hold-weight position of the crossfade used for the most recent frame
    /// group: 0 = live only, 1 = hold only, stepping by 1/crossfade_frames
    /// per frame group. Engage mixes hold/live linearly at this position;
    /// release mixes them with equal-power gains sin/cos(progress * pi/2).
    /// 0 while not latched. `engage_progress() == 1 && !is_releasing()`
    /// means the last frame group was pure hold.
    SampleType engage_progress() const { return latched_ ? fade_ : SampleType{0}; }

    // ── held-state pipeline ────────────────────────────────────────────────

    /// Averaged magnitudes of one channel's hold (num_bins values).
    std::span<const SampleType> held_magnitudes(int channel) const {
        return {held_mag_.data() + static_cast<size_t>(channel) * static_cast<size_t>(num_bins_),
                static_cast<size_t>(num_bins_)};
    }
    /// Current hold phases of one channel, wrapped to [-pi, pi]: the phases
    /// the next hold frame is rendered with.
    std::span<const double> held_phases(int channel) const {
        return {held_phase_.data() + static_cast<size_t>(channel) * static_cast<size_t>(num_bins_),
                static_cast<size_t>(num_bins_)};
    }
    /// Per-bin instantaneous frequency of the hold, radians per sample.
    std::span<const double> instantaneous_frequency() const {
        return {inst_freq_.data(), inst_freq_.size()};
    }

    /// Render the current hold frame (magnitude and phase only — no live
    /// mix, no crossfade, no phase advance) into `frames`. Returns false,
    /// writing nothing, when there is no hold or the shape does not match.
    bool write_hold(std::complex<SampleType>* const* frames, int channels,
                    int num_bins) const {
        if (!has_hold_ || channels != config_.channels || num_bins != num_bins_)
            return false;
        for (int ch = 0; ch < channels; ++ch) {
            const auto mags = held_magnitudes(ch);
            const auto phases = held_phases(ch);
            for (int k = 0; k < num_bins; ++k)
                frames[ch][k] = std::polar(mags[static_cast<size_t>(k)],
                                           static_cast<SampleType>(phases[static_cast<size_t>(k)]));
        }
        return true;
    }

    /// Advance the hold by `frames` analysis hops exactly as process_group()
    /// does after each held frame (instantaneous frequency plus the random
    /// walk, consuming the PRNG). For consumers that render the hold with
    /// write_hold() instead of process_group(); never combine the two on
    /// the same frame.
    void advance_hold(int frames = 1) {
        for (int i = 0; i < frames; ++i) advance_hold_phases();
    }

    /// Move the hold phases back by `frames` hops at the instantaneous
    /// frequency, without the random walk and without touching the PRNG —
    /// e.g. to align a hold with a path that runs a fixed number of hops
    /// behind it. Not an exact inverse of advance_hold(), whose walk it
    /// does not undo.
    void rewind_hold_phases(int frames) {
        const double hops = static_cast<double>(frames)
                          * static_cast<double>(config_.analysis_hop);
        for (int k = 0; k < num_bins_; ++k) {
            const double back = inst_freq_[static_cast<size_t>(k)] * hops;
            for (int ch = 0; ch < config_.channels; ++ch) {
                double& phase = held_phase_[static_cast<size_t>(ch) * num_bins_
                                            + static_cast<size_t>(k)];
                phase = princarg(phase - back);
            }
        }
    }

    // ── state recall ───────────────────────────────────────────────────────

    /// Copy the current hold into `out`, which must have been prepared for
    /// this geometry. Returns false (leaving `out` unchanged) when there is
    /// no hold or the storage does not match. Allocation-free.
    bool snapshot(Snapshot& out) const {
        const auto bins = static_cast<std::size_t>(num_bins_);
        const auto channel_bins = static_cast<std::size_t>(config_.channels) * bins;
        if (!has_hold_ || num_bins_ == 0 || out.fft_size != config_.fft_size
            || out.channels != config_.channels
            || out.analysis_hop != config_.analysis_hop
            || out.magnitudes.size() != channel_bins
            || out.phases.size() != channel_bins || out.inst_freq.size() != bins)
            return false;
        std::copy(held_mag_.begin(), held_mag_.end(), out.magnitudes.begin());
        std::copy(held_phase_.begin(), held_phase_.end(), out.phases.begin());
        std::copy(inst_freq_.begin(), inst_freq_.end(), out.inst_freq.begin());
        out.capture_frames = latched_capture_frames_;
        out.crossfade_frames = config_.crossfade_frames;
        out.fade_step = latched_ ? fade_step_ : 0;
        out.engaged = latched_ && engaged_ && !releasing_;
        out.releasing = latched_ && releasing_;
        out.sample_rate = config_.sample_rate;
        out.rng_state = rng_;
        return true;
    }

    /// Stage `in` to become the hold at the start of the next
    /// process_group(). Safe to call from ONE non-audio thread concurrently
    /// with process_group(); copies into storage allocated at prepare().
    /// Returns false — the caller should drop the capture and re-arm — for a
    /// snapshot whose fft_size, channels or analysis_hop differ from this
    /// hold's, whose known sample rate differs from this hold's known rate,
    /// an invalid snapshot, or when an earlier restore has not been adopted
    /// yet. `crossfade` and `immediate` adoption engage the hold
    /// (set_frozen(true) is implied); `as_captured` resumes the recorded
    /// request and fade position.
    bool stage_restore(const Snapshot& in,
                       FreezeRestoreEngage engage = FreezeRestoreEngage::crossfade) {
        if (num_bins_ == 0 || in.fft_size != config_.fft_size
            || in.channels != config_.channels
            || in.analysis_hop != config_.analysis_hop || !in.valid()
            || (in.sample_rate > 0.0 && config_.sample_rate > 0.0
                && in.sample_rate != config_.sample_rate))
            return false;
        if (pending_.value.load(std::memory_order_acquire) != kRestoreIdle)
            return false;
        std::copy(in.magnitudes.begin(), in.magnitudes.end(), pending_mag_.begin());
        std::copy(in.phases.begin(), in.phases.end(), pending_phase_.begin());
        std::copy(in.inst_freq.begin(), in.inst_freq.end(), pending_inst_freq_.begin());
        pending_rng_ = in.rng_state;
        const int steps = config_.crossfade_frames;
        switch (engage) {
            case FreezeRestoreEngage::immediate:
                pending_engaged_ = true;
                pending_releasing_ = false;
                pending_fade_step_ = steps;
                break;
            case FreezeRestoreEngage::as_captured:
                pending_engaged_ = in.engaged;
                pending_releasing_ = in.releasing;
                // Same fraction of this hold's crossfade.
                pending_fade_step_ = static_cast<int>(std::lround(
                    static_cast<double>(in.fade_step) * steps / in.crossfade_frames));
                break;
            case FreezeRestoreEngage::crossfade:
            default:
                pending_engaged_ = true;
                pending_releasing_ = false;
                pending_fade_step_ = 0;
                break;
        }
        pending_.value.store(kRestoreReady, std::memory_order_release);
        return true;
    }

    /// True while a staged restore is waiting for the next frame group.
    /// Safe from any thread.
    bool restore_pending() const {
        return pending_.value.load(std::memory_order_acquire) == kRestoreReady;
    }

    // ── streaming ──────────────────────────────────────────────────────────

    /// Run at the head of the vocoder chain for every analysis frame
    /// group. Captures live frames; when engaged and latched, replaces
    /// `frames` with hold content (crossfaded at the edges).
    void process_group(std::complex<SampleType>* const* frames, int channels, int num_bins) {
        assert(num_bins == num_bins_);
        assert(channels == config_.channels);

        if (pending_.value.load(std::memory_order_acquire) == kRestoreReady)
            adopt_restore();

        // Capture BEFORE anything below rewrites `frames`: the window must
        // hold live input, never a mix that already contains the hold.
        // Capture runs whenever no hold is being sustained — including the
        // release fade, so a re-freeze latches on post-release input as soon
        // as the fresh window is full.
        if (!latched_ || releasing_)
            capture(frames, channels);

        if (releasing_ && fade_step_ <= 0) { // release completed last frame
            latched_ = false;
            releasing_ = false;
        }

        if (engaged_ && !latched_ && captured_ >= active_capture_frames_)
            latch();

        if (!latched_)
            return; // pass-through (possibly still filling — never mute)

        // Crossfade target: 1 while holding, 0 while releasing.
        //
        // The fade LAW depends on direction. At ENGAGE the hold starts
        // phase-aligned with the live signal (initialized from the latched
        // frame and advanced only AFTER each mix, so the latch frame plays
        // its captured phases verbatim): the sum is correlated, and linear
        // gains keep it amplitude-flat — equal-power would bump it up to
        // 3 dB. At RELEASE the hold has drifted (instantaneous-frequency
        // estimate error + the deliberate phase random-walk), so hold and
        // live are decorrelated: there a linear fade dips ~3–5 dB mid-fade
        // (measured -5.1 dB on a steady tone — an audible "ding"), and the
        // power-flat equal-power law is correct.
        //
        // The position is an integer step count: an accumulated 1/N float
        // step does not reach exactly 1 for every N (N = 12 stops at
        // 0.99999988), which would leave a residual live leak forever.
        const int steps = config_.crossfade_frames;
        fade_step_ = std::clamp(fade_step_ + (releasing_ ? -1 : 1), 0, steps);
        fade_ = static_cast<SampleType>(fade_step_) / static_cast<SampleType>(steps);
        SampleType hold_gain, live_gain;
        if (!releasing_) {
            hold_gain = fade_;
            live_gain = SampleType{1} - fade_;
        } else {
            const SampleType t = fade_ * static_cast<SampleType>(1.57079632679489662); // fade_ * pi/2
            hold_gain = std::sin(t);
            live_gain = std::cos(t);
        }

        for (int ch = 0; ch < channels; ++ch) {
            const SampleType* mags = held_mag_.data()
                                     + static_cast<size_t>(ch) * num_bins_;
            const double* phases = held_phase_.data()
                                   + static_cast<size_t>(ch) * num_bins_;
            // Per-channel frame energies for the transition normalization
            // below: the gain laws above are only flat on average — a
            // narrowband bin whose hold drifted to anti-phase with the live
            // signal partially cancels under ANY fixed-gain crossfade
            // (measured: a residual -2.7 dB release notch on a pure tone).
            // Renormalizing the mixed frame to the power-interpolated
            // target makes the transition level-flat by construction, for
            // any content and any phase relationship.
            double e_live = 0.0, e_hold = 0.0, e_mix = 0.0;
            for (int k = 0; k < num_bins_; ++k) {
                const auto live = frames[ch][k];
                const auto held = std::polar(mags[k] * hold_gain,
                                             static_cast<SampleType>(phases[k]));
                e_live += static_cast<double>(std::norm(live));
                e_hold += static_cast<double>(mags[k]) * mags[k];
                const auto mixed = live * live_gain + held;
                e_mix += static_cast<double>(std::norm(mixed));
                frames[ch][k] = mixed;
            }
            if (live_gain > SampleType{0}) { // mid-fade only; steady hold is exact
                // Target: endpoint energies interpolated by fade POSITION,
                // not by the gains — gain-derived targets are wrong for one
                // correlation case or the other (a g²-weighted target undid
                // the correlated engage fade by up to -3 dB mid-fade).
                const double target =
                    (1.0 - static_cast<double>(fade_)) * e_live
                    + static_cast<double>(fade_) * e_hold;
                const double scale_sq = target / std::max(e_mix, 1e-12);
                const SampleType scale = static_cast<SampleType>(std::sqrt(
                    std::clamp(scale_sq, 0.0625, 16.0)));
                for (int k = 0; k < num_bins_; ++k) frames[ch][k] *= scale;
            }
        }
        // Advance AFTER mixing: the first held frame must reproduce the
        // latched phases exactly, or the engage fade sums partially
        // cancelling signals (one-hop phase skew ≈ arbitrary per bin —
        // measured as a -4.6 dB dip at engage before this ordering).
        advance_hold_phases();
    }

private:
    void capture(std::complex<SampleType>* const* frames, int channels) {
        const auto bins = static_cast<size_t>(num_bins_);
        const auto depth = static_cast<size_t>(config_.max_capture_frames);
        for (int ch = 0; ch < channels; ++ch) {
            SampleType* slot = capture_mag_.data()
                               + (static_cast<size_t>(ch) * depth
                                  + static_cast<size_t>(capture_pos_)) * bins;
            for (int k = 0; k < num_bins_; ++k)
                slot[k] = std::abs(frames[ch][k]);
        }
        // Per-bin phase increment since the previous captured frame, from
        // all channels at once: arg(sum_ch X_t * conj(X_{t-1})). The
        // magnitude weighting follows the dominant channel, and unlike the
        // phase of the channel SUM it is not destroyed where channels
        // cancel (anti-phase bins). The first frame after a history clear
        // has no predecessor and records the bin-centre increment.
        double* increment = latest_increment_.data();
        const double ha = static_cast<double>(config_.analysis_hop);
        for (int k = 0; k < num_bins_; ++k) {
            std::complex<double> acc(0.0, 0.0);
            for (int ch = 0; ch < channels; ++ch) {
                const auto now = frames[ch][k];
                auto& prev = prev_frame_[static_cast<size_t>(ch) * bins + static_cast<size_t>(k)];
                acc += std::complex<double>(now) * std::conj(std::complex<double>(prev));
                prev = now;
            }
            increment[k] = prev_valid_ ? std::arg(acc) : two_pi_ * k / config_.fft_size * ha;
        }
        prev_valid_ = true;
        last_frames_ = frames; // valid only within process_group call
        capture_pos_ = (capture_pos_ + 1) % config_.max_capture_frames;
        if (captured_ < config_.max_capture_frames) ++captured_;
    }

    void latch() {
        const auto bins = static_cast<size_t>(num_bins_);
        const int ring = config_.max_capture_frames;
        const int count = active_capture_frames_;

        // Hold magnitudes: average over the newest `count` captured frames —
        // exactly the last `count` consecutive analysis frames.
        for (int ch = 0; ch < config_.channels; ++ch) {
            SampleType* held = held_mag_.data() + static_cast<size_t>(ch) * bins;
            std::fill(held, held + bins, SampleType{0});
            for (int back = 1; back <= count; ++back) {
                const auto slot_index = static_cast<size_t>((capture_pos_ - back + ring) % ring);
                const SampleType* slot = capture_mag_.data()
                    + (static_cast<size_t>(ch) * static_cast<size_t>(ring) + slot_index) * bins;
                for (int k = 0; k < num_bins_; ++k) held[k] += slot[k];
            }
            const SampleType inv = SampleType{1} / static_cast<SampleType>(count);
            for (int k = 0; k < num_bins_; ++k) held[k] *= inv;
        }
        latched_capture_frames_ = count;

        // Instantaneous frequency from the newest capture's increment
        // (heterodyned phase increment over one analysis hop).
        const double* increment = latest_increment_.data();
        const double ha = static_cast<double>(config_.analysis_hop);
        for (int k = 0; k < num_bins_; ++k) {
            const double omega = two_pi_ * k / config_.fft_size;
            const double delta = princarg(increment[k] - omega * ha);
            inst_freq_[static_cast<size_t>(k)] = omega + delta / ha;
        }

        // Initial hold phases from the latched (newest) live frame so the
        // per-channel phase relationships — the image — carry into the hold.
        if (last_frames_ != nullptr) {
            for (int ch = 0; ch < config_.channels; ++ch) {
                double* phases = held_phase_.data() + static_cast<size_t>(ch) * bins;
                for (int k = 0; k < num_bins_; ++k)
                    phases[k] = static_cast<double>(std::arg(last_frames_[ch][k]));
            }
        }
        fade_ = SampleType{0};
        fade_step_ = 0;
        latched_ = true;
        has_hold_ = true;
    }

    void adopt_restore() {
        std::swap(held_mag_, pending_mag_);
        std::swap(held_phase_, pending_phase_);
        std::swap(inst_freq_, pending_inst_freq_);
        rng_ = pending_rng_;
        engaged_ = pending_engaged_;
        releasing_ = pending_releasing_;
        // A hold recorded after its release completed is kept (has_hold())
        // but not played.
        latched_ = engaged_ || releasing_;
        has_hold_ = true;
        fade_step_ = pending_fade_step_;
        fade_ = static_cast<SampleType>(fade_step_)
              / static_cast<SampleType>(config_.crossfade_frames);
        if (releasing_) captured_ = 0; // the next hold starts from fresh input
        pending_.value.store(kRestoreIdle, std::memory_order_release);
    }

    void advance_hold_phases() {
        const double ha = static_cast<double>(config_.analysis_hop);
        const double jitter = static_cast<double>(config_.phase_jitter);
        for (int k = 0; k < num_bins_; ++k) {
            const double advance = inst_freq_[static_cast<size_t>(k)] * ha
                                   + jitter * next_uniform();
            // Keep phases wrapped: the mix converts them to SampleType, and an
            // unbounded float phase loses the per-channel offsets (the stereo
            // image) and inter-frame coherence within seconds of holding.
            for (int ch = 0; ch < config_.channels; ++ch) {
                double& phase = held_phase_[static_cast<size_t>(ch) * num_bins_
                                            + static_cast<size_t>(k)];
                phase = princarg(phase + advance);
            }
        }
    }

    // xorshift64* — deterministic, allocation-free; uniform in [-1, 1].
    double next_uniform() {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const std::uint64_t r = rng_ * 0x2545f4914f6cdd1dull;
        return static_cast<double>(r >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }

    static double princarg(double p) {
        return p - two_pi_ * std::round(p / two_pi_);
    }

    static constexpr double two_pi_ = 6.28318530717958647692;
    static constexpr std::uint64_t kRngSeed = 0x9e3779b97f4a7c15ull;
    static constexpr std::uint32_t kRestoreIdle = 0;
    static constexpr std::uint32_t kRestoreReady = 1;

    // Copyable wrapper so the hold (and the processors that own one by value)
    // keep their implicit copy/move operations.
    struct RestoreFlag {
        std::atomic<std::uint32_t> value{kRestoreIdle};
        RestoreFlag() = default;
        RestoreFlag(const RestoreFlag& other) noexcept
            : value(other.value.load(std::memory_order_acquire)) {}
        RestoreFlag& operator=(const RestoreFlag& other) noexcept {
            value.store(other.value.load(std::memory_order_acquire),
                        std::memory_order_release);
            return *this;
        }
    };

    Config config_;
    int num_bins_ = 0;

    std::vector<SampleType> capture_mag_;          // channels * max depth * bins
    std::vector<double> latest_increment_;         // bins
    std::vector<std::complex<SampleType>> prev_frame_; // channels * bins
    std::vector<SampleType> held_mag_;             // channels * bins
    std::vector<double> held_phase_;               // channels * bins, wrapped
    std::vector<double> inst_freq_;                // bins

    // Staged-restore slot (single non-audio producer, audio consumer).
    std::vector<SampleType> pending_mag_;
    std::vector<double> pending_phase_;
    std::vector<double> pending_inst_freq_;
    std::uint64_t pending_rng_ = 0;
    int pending_fade_step_ = 0;
    bool pending_engaged_ = false;
    bool pending_releasing_ = false;
    RestoreFlag pending_;

    std::complex<SampleType>* const* last_frames_ = nullptr;
    int captured_ = 0;
    int capture_pos_ = 0;
    int active_capture_frames_ = 2;
    int latched_capture_frames_ = 0;
    bool prev_valid_ = false;
    bool engaged_ = false;
    bool latched_ = false;
    bool releasing_ = false;
    bool has_hold_ = false;
    SampleType fade_ = SampleType{0};
    int fade_step_ = 0;
    std::uint64_t rng_ = kRngSeed;
};

using FreezeHold = FreezeHoldT<float>;
using FreezeHold64 = FreezeHoldT<double>;

} // namespace pulp::signal
