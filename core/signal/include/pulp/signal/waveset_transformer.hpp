#pragma once
#include "pulp/signal/crossfade.hpp"
#include "pulp/signal/rng.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
namespace pulp::signal {
namespace detail {
struct CapacityLayout {
    std::size_t n{}, m{}, wcap{}, e{}, q{}, a{}, g{}, l{}, f{}, bytes{};
    static bool add(std::size_t a, std::size_t b, std::size_t& result) noexcept {
        if (b > std::numeric_limits<std::size_t>::max() - a)
            return false;
        result = a + b;
        return true;
    }
    static bool multiply(std::size_t a, std::size_t b, std::size_t& result) noexcept {
        if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
            return false;
        result = a * b;
        return true;
    }
    static bool make(double sample_rate, std::size_t n, std::size_t m, double normalize_ratio,
                     std::size_t repeat_limit, std::size_t rotate_limit, std::size_t slot_count,
                     std::size_t sample_bytes, std::size_t held_bytes, std::size_t token_bytes,
                     std::size_t program_bytes, std::size_t step_bytes,
                     CapacityLayout& result) noexcept {
        CapacityLayout candidate{};
        candidate.n = n;
        candidate.m = m;
        candidate.wcap = std::min(n, rotate_limit);
        std::size_t repeat_bound{}, normalize_bound{};
        if (!multiply(m, repeat_limit, repeat_bound))
            return false;
        const long double normalized = static_cast<long double>(m) * normalize_ratio;
        if (!std::isfinite(normalized) ||
            normalized > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
            return false;
        normalize_bound = static_cast<std::size_t>(std::ceil(normalized));
        candidate.e = std::max(repeat_bound, normalize_bound);
        const long double fade_product = static_cast<long double>(sample_rate) * 0.020L;
        if (!std::isfinite(fade_product) ||
            fade_product > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
            return false;
        candidate.f = static_cast<std::size_t>(std::ceil(fade_product));
        if ((candidate.f & 1u) != 0u && !add(candidate.f, 1u, candidate.f))
            return false;
        std::size_t n1{};
        if (!add(n, 1u, n1) || !multiply(n1, candidate.e, candidate.q) ||
            !add(candidate.q, candidate.f, candidate.q) ||
            !multiply(candidate.wcap, m, candidate.a) || !multiply(n, m, candidate.g) ||
            !add(candidate.a, candidate.g, candidate.l))
            return false;
        std::size_t sample_count{}, term{}, count{};
        if (!add(m, candidate.e, sample_count) || !add(sample_count, candidate.q, sample_count) ||
            !add(sample_count, candidate.g, sample_count) ||
            !multiply(sample_count, sample_bytes, candidate.bytes) ||
            !multiply(n, held_bytes, term) || !add(candidate.bytes, term, candidate.bytes) ||
            !multiply(n, token_bytes, term) || !add(candidate.bytes, term, candidate.bytes) ||
            !multiply(slot_count, program_bytes, term) ||
            !add(candidate.bytes, term, candidate.bytes) || !multiply(slot_count, n, count) ||
            !multiply(count, step_bytes, term) || !add(candidate.bytes, term, candidate.bytes) ||
            !multiply(slot_count, candidate.wcap, count) ||
            !multiply(count, sizeof(std::uint16_t), term) ||
            !add(candidate.bytes, term, candidate.bytes))
            return false;
        result = candidate;
        return true;
    }
};
struct ReservationToken {
    enum class Owner : std::uint8_t { Free, Capture, Rotate };
    Owner owner = Owner::Free;
};
template <typename Program, typename Step, std::size_t Slots> class ProgramBank {
  public:
    bool prepare(std::size_t n, std::size_t wcap) noexcept {
        auto programs = std::unique_ptr<Program[]>(new (std::nothrow) Program[Slots]);
        auto steps = std::unique_ptr<Step[]>(new (std::nothrow) Step[Slots * n]);
        auto permutations =
            std::unique_ptr<std::uint16_t[]>(new (std::nothrow) std::uint16_t[Slots * wcap]);
        if (!programs || !steps || !permutations)
            return false;
        programs_ = std::move(programs);
        steps_ = std::move(steps);
        permutations_ = std::move(permutations);
        n_ = n;
        wcap_ = wcap;
        return true;
    }
    Program& operator[](std::size_t index) noexcept {
        return programs_[index];
    }
    const Program& operator[](std::size_t index) const noexcept {
        return programs_[index];
    }
    Step& step(std::size_t slot, std::size_t index) noexcept {
        return steps_[slot * n_ + index];
    }
    const Step& step(std::size_t slot, std::size_t index) const noexcept {
        return steps_[slot * n_ + index];
    }
    std::uint16_t& permutation(std::size_t slot, std::size_t index) noexcept {
        return permutations_[slot * wcap_ + index];
    }
    template <typename SourceProgram, typename Validator>
    bool publish(std::size_t slot, const SourceProgram& source, Validator&& validate) noexcept {
        if (slot >= Slots || !validate(source))
            return false;
        for (std::size_t i = 0; i < source.steps.size(); ++i)
            step(slot, i) = source.steps[i];
        for (std::size_t i = 0; i < source.permutation.size(); ++i)
            permutation(slot, i) = source.permutation[i];
        auto& destination = programs_[slot];
        destination.version = source.version;
        destination.step_count = source.steps.size();
        destination.rotate_window = source.rotate_window;
        destination.permutation_count = source.permutation.size();
        destination.configured = true;
        return true;
    }
    const std::uint16_t& permutation(std::size_t slot, std::size_t index) const noexcept {
        return permutations_[slot * wcap_ + index];
    }
    void clear() noexcept {
        programs_.reset();
        steps_.reset();
        permutations_.reset();
        n_ = wcap_ = 0;
    }

  private:
    std::unique_ptr<Program[]> programs_;
    std::unique_ptr<Step[]> steps_;
    std::unique_ptr<std::uint16_t[]> permutations_;
    std::size_t n_ = 0, wcap_ = 0;
};
template <typename Sample> class Segmenter {
  public:
    enum class Side : std::uint8_t { Negative, Inside, Positive };
    Side classify(Sample sample, Sample epsilon) const noexcept {
        if (sample < -epsilon)
            return Side::Negative;
        if (sample > epsilon)
            return Side::Positive;
        return Side::Inside;
    }
    bool transition(Sample sample, Sample epsilon, std::uint8_t polarity) const noexcept {
        const auto next = classify(sample, epsilon);
        if (next == Side::Inside || last_ == Side::Inside || next == last_)
            return false;
        const bool rising = last_ == Side::Negative && next == Side::Positive;
        return polarity == 2u || (polarity == 0u && rising) || (polarity == 1u && !rising);
    }
    void observe(Sample sample, Sample epsilon) noexcept {
        const auto side = classify(sample, epsilon);
        if (side != Side::Inside)
            last_ = side;
    }
    void reset() noexcept {
        last_ = Side::Inside;
    }

  private:
    Side last_ = Side::Inside;
};
class ReservationLedger {
  public:
    bool prepare(std::size_t count) noexcept {
        auto tokens =
            std::unique_ptr<ReservationToken[]>(new (std::nothrow) ReservationToken[count]);
        if (!tokens)
            return false;
        tokens_ = std::move(tokens);
        capacity_ = count;
        reset();
        return true;
    }
    void reset() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i)
            tokens_[i].owner = ReservationToken::Owner::Free;
        count_ = 0;
    }
    std::size_t acquire_capture() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (tokens_[i].owner == ReservationToken::Owner::Free) {
                tokens_[i].owner = ReservationToken::Owner::Capture;
                ++count_;
                return i;
            }
        }
        return capacity_;
    }
    bool transfer_to_rotate(std::size_t index) noexcept {
        if (index >= capacity_ || tokens_[index].owner != ReservationToken::Owner::Capture)
            return false;
        tokens_[index].owner = ReservationToken::Owner::Rotate;
        return true;
    }
    bool release(std::size_t index, ReservationToken::Owner owner) noexcept {
        if (index >= capacity_ || tokens_[index].owner != owner)
            return false;
        tokens_[index].owner = ReservationToken::Owner::Free;
        --count_;
        return true;
    }
    bool owns(std::size_t index, ReservationToken::Owner owner) const noexcept {
        return index < capacity_ && tokens_[index].owner == owner;
    }
    std::size_t count() const noexcept {
        return count_;
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }
    void clear() noexcept {
        tokens_.reset();
        capacity_ = count_ = 0;
    }

  private:
    std::unique_ptr<ReservationToken[]> tokens_;
    std::size_t capacity_ = 0, count_ = 0;
};
template <typename Sample> class OutputSplicer {
  public:
    bool prepare(std::size_t capacity) noexcept {
        auto fifo = std::unique_ptr<Sample[]>(new (std::nothrow) Sample[capacity]);
        if (!fifo)
            return false;
        fifo_ = std::move(fifo);
        capacity_ = capacity;
        reset();
        return true;
    }
    void reset() noexcept {
        head_ = size_ = 0;
        invalidate();
    }
    void clear() noexcept {
        fifo_.reset();
        capacity_ = 0;
        reset();
    }
    void publish(std::size_t full_length) noexcept {
        valid_ = true;
        full_length_ = full_length;
        age_ = 0;
    }
    bool age(std::size_t limit) noexcept {
        if (!valid_)
            return false;
        if (++age_ == limit) {
            invalidate();
            return true;
        }
        return false;
    }
    void invalidate() noexcept {
        valid_ = false;
        full_length_ = age_ = 0;
    }
    bool valid() const noexcept {
        return valid_;
    }
    std::size_t full_length() const noexcept {
        return full_length_;
    }
    std::size_t size() const noexcept {
        return size_;
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }
    std::size_t pull(Sample* output, std::size_t count, std::size_t holdback) noexcept {
        const auto available = size_ > holdback ? size_ - holdback : 0u;
        const auto amount = std::min(available, count);
        for (std::size_t i = 0; i < amount; ++i) {
            output[i] = fifo_[head_];
            head_ = (head_ + 1u) % capacity_;
        }
        size_ -= amount;
        if (size_ == 0)
            invalidate();
        return amount;
    }
    void append(const Sample* samples, std::size_t length, std::size_t fade,
                CrossfadeGainLaw law) noexcept {
        for (std::size_t i = 0; i < fade; ++i) {
            const auto position = (head_ + size_ - fade + i) % capacity_;
            const auto t =
                fade == 1 ? Sample{1} : static_cast<Sample>(i) / static_cast<Sample>(fade - 1u);
            Sample old_gain{}, new_gain{};
            crossfade_gains(crossfade_smoothstep(t), law, old_gain, new_gain);
            fifo_[position] = fifo_[position] * old_gain + samples[i] * new_gain;
        }
        for (std::size_t i = fade; i < length; ++i) {
            fifo_[(head_ + size_) % capacity_] = samples[i];
            ++size_;
        }
        publish(length);
    }

  private:
    std::unique_ptr<Sample[]> fifo_;
    std::size_t capacity_ = 0, head_ = 0, size_ = 0;
    std::size_t full_length_ = 0, age_ = 0;
    bool valid_ = false;
};
} // namespace detail
/**
 * A bounded, allocation-free-at-runtime waveset transformation engine.
 *
 * Call prepare() and set_program() only from a control thread; both may allocate or copy. After
 * preparation, push(), pull(), finish_input(), and the parameter setters are suitable for the
 * realtime audio thread and perform no allocation or blocking synchronization. One audio thread
 * owns push()/pull()/finish_input()/reset(); control-thread publication may race with that owner
 * only through the documented setters and program-slot requests. Those operations fail closed by
 * returning false when a concurrent lifecycle transition owns the packed control state.
 *
 * push() may accept fewer samples than requested when its bounded reservation/output capacity is
 * exhausted. The caller must pull available output and retry the unaccepted suffix. Output remains
 * owned by the transformer until pull() consumes it, and drained() becomes true only after input
 * has finished and all buffered output and reservations have retired.
 */
template <typename SampleType = float>
    requires std::is_floating_point_v<SampleType>
class WavesetTransformerT {
  public:
    static constexpr std::size_t kMaxProgramSlots = 8;
    static constexpr std::size_t kMaxCoordinateChoices = 8;
    static constexpr int kMaxRepeatCount = 16;
    static constexpr int kMaxRotateWindow = 256;
    struct Capacity {
        int max_wavesets = 0;
        int max_waveset_samples = 0;
        double max_normalize_ratio = 0.0;
    };
    enum class ZeroCrossingPolarity : std::uint8_t { Rising, Falling, Both };
    enum class Operation : std::uint8_t {
        Pass,
        Repeat,
        Omit,
        Reverse,
        Rotate,
        Normalize,
        CoordinateSelect
    };
    struct ProgramStep {
        Operation operation = Operation::Pass;
        std::uint8_t repeat_count = 1;
        std::array<Operation, kMaxCoordinateChoices> coordinate_choices{};
        std::uint8_t coordinate_choice_count = 0;
    };
    struct OperationProgram {
        std::uint32_t version = 1;
        std::vector<ProgramStep> steps;
        int rotate_window = 1;
        std::vector<std::uint16_t> permutation;
    };
    bool prepare(double sample_rate, Capacity capacity) {
        if (prepared())
            return false;
        clear_configuration();
        if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || capacity.max_wavesets <= 0 ||
            capacity.max_waveset_samples <= 0 || !std::isfinite(capacity.max_normalize_ratio) ||
            capacity.max_normalize_ratio < 1.0)
            return false;
        detail::CapacityLayout layout{};
        if (!detail::CapacityLayout::make(
                sample_rate, static_cast<std::size_t>(capacity.max_wavesets),
                static_cast<std::size_t>(capacity.max_waveset_samples),
                capacity.max_normalize_ratio, static_cast<std::size_t>(kMaxRepeatCount),
                static_cast<std::size_t>(kMaxRotateWindow), kMaxProgramSlots, sizeof(SampleType),
                sizeof(HeldSegment), sizeof(detail::ReservationToken), sizeof(StoredProgram),
                sizeof(ProgramStep), layout) ||
            layout.l > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return false;
        const auto n = layout.n;
        const auto m = layout.m;
        struct PreparedStorage {
            std::unique_ptr<SampleType[]> capture, scratch, rotate;
            std::unique_ptr<HeldSegment[]> held;
            detail::ProgramBank<StoredProgram, ProgramStep, kMaxProgramSlots> programs;
            detail::ReservationLedger ledger;
            detail::OutputSplicer<SampleType> splicer;
        } storage;
        storage.capture.reset(new (std::nothrow) SampleType[m]);
        storage.scratch.reset(new (std::nothrow) SampleType[layout.e]);
        storage.rotate.reset(new (std::nothrow) SampleType[layout.g]);
        storage.held.reset(new (std::nothrow) HeldSegment[n]);
        if (!storage.programs.prepare(n, layout.wcap) || !storage.ledger.prepare(n) ||
            !storage.splicer.prepare(layout.q) || !storage.capture || !storage.scratch ||
            !storage.rotate || !storage.held)
            return false;
        capture_ = std::move(storage.capture);
        scratch_ = std::move(storage.scratch);
        rotate_storage_ = std::move(storage.rotate);
        held_ = std::move(storage.held);
        programs_ = std::move(storage.programs);
        ledger_ = std::move(storage.ledger);
        splicer_ = std::move(storage.splicer);
        sample_rate_ = sample_rate;
        capacity_ = capacity;
        max_normalize_ratio_ = capacity.max_normalize_ratio;
        max_segment_output_ = layout.e;
        max_lookahead_ = static_cast<int>(layout.l);
        tail_ = layout.q;
        max_fade_ = layout.f;
        programs_[0].version = 1;
        programs_[0].step_count = 1;
        programs_.step(0, 0) = ProgramStep{};
        programs_[0].rotate_window = 1;
        programs_[0].permutation_count = 0;
        programs_[0].configured = true;
        control_.store(pack_control(State::Quiescent, 0, 1u), std::memory_order_release);
        reset();
        return true;
    }
    bool prepared() const noexcept {
        return control_state(control_.load(std::memory_order_acquire)) != State::Unprepared;
    }
    bool set_program(std::uint8_t slot, const OperationProgram& program) {
        if (slot >= kMaxProgramSlots)
            return false;
        auto word = control_.load(std::memory_order_acquire);
        for (;;) {
            if (control_state(word) != State::Quiescent)
                return false;
            const auto publishing = with_state(word, State::Publishing);
            if (control_.compare_exchange_weak(word, publishing, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
#if defined(PULP_WAVESET_TEST_SEAMS)
                if (const auto hook = publication_hook_.load(std::memory_order_acquire))
                    hook();
#endif
                const bool published =
                    programs_.publish(slot, program, [this](const auto& candidate) noexcept {
                        return validate_program(candidate);
                    });
                auto published_control = control_.load(std::memory_order_acquire);
                for (;;) {
                    auto configured = control_configured(published_control);
                    if (published)
                        configured |= std::uint32_t{1} << slot;
                    const auto next_state = control_finish_requested(published_control)
                                                ? State::Finished
                                                : State::Quiescent;
                    const auto completed =
                        pack_control(next_state, control_slot(published_control), configured);
                    if (control_.compare_exchange_weak(published_control, completed,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
                        break;
                }
                return published;
            }
        }
    }
    bool request_program_slot(std::uint8_t slot) noexcept {
        if (slot >= kMaxProgramSlots)
            return false;
        auto word = control_.load(std::memory_order_acquire);
        for (;;) {
            const auto state = control_state(word);
            if (control_finish_requested(word) ||
                (state != State::Quiescent && state != State::Live && state != State::Pushing) ||
                (control_configured(word) & (std::uint32_t{1} << slot)) == 0)
                return false;
            const auto desired =
                pack_control(state, slot, control_configured(word)) | (word & kFinishRequestedMask);
            if (control_.compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                return true;
        }
    }
    bool set_zero_crossing_polarity(ZeroCrossingPolarity polarity) noexcept {
        if (polarity < ZeroCrossingPolarity::Rising || polarity > ZeroCrossingPolarity::Both)
            return false;
        return update_control([&] { polarity_.store(static_cast<std::uint8_t>(polarity)); });
    }
    bool set_zero_crossing_epsilon(SampleType epsilon) noexcept {
        if (!std::isfinite(epsilon) || epsilon < SampleType{})
            return false;
        return update_control([&] { epsilon_.store(epsilon); });
    }
    bool set_crossfade_law(CrossfadeGainLaw law) noexcept {
        if (law != CrossfadeGainLaw::EqualGain && law != CrossfadeGainLaw::EqualPower)
            return false;
        return update_control([&] { crossfade_law_.store(static_cast<std::uint8_t>(law)); });
    }
    bool set_crossfade_duration_ms(SampleType duration_ms) noexcept {
        if (!std::isfinite(duration_ms) || duration_ms < SampleType{} ||
            duration_ms > SampleType{20})
            return false;
        return update_control([&] { crossfade_ms_.store(duration_ms); });
    }
    bool set_normalize_ratio(SampleType ratio) noexcept {
        if (!std::isfinite(ratio) || ratio <= SampleType{} ||
            static_cast<double>(ratio) > max_normalize_ratio_)
            return false;
        return update_control([&] { normalize_ratio_.store(ratio); });
    }
#if defined(PULP_WAVESET_TEST_SEAMS)
    using StartupHook = void (*)();
    static void set_startup_hook(StartupHook hook) noexcept {
        startup_hook_.store(hook);
    }
    using PublicationHook = void (*)();
    static void set_publication_hook(PublicationHook hook) noexcept {
        publication_hook_.store(hook);
    }
    using FinishHook = void (*)();
    static void set_finish_hook(FinishHook hook) noexcept {
        finish_hook_.store(hook);
    }
    using ResetHook = void (*)();
    static void set_reset_hook(ResetHook hook) noexcept {
        reset_hook_.store(hook);
    }
    using ResetCompletionHook = void (*)();
    static void set_reset_completion_hook(ResetCompletionHook hook) noexcept {
        reset_completion_hook_.store(hook);
    }
    using ControlCompletionHook = void (*)();
    static void set_control_completion_hook(ControlCompletionHook hook) noexcept {
        control_completion_hook_.store(hook);
    }
    using DrainedSnapshotHook = void (*)();
    static void set_drained_snapshot_hook(DrainedSnapshotHook hook) noexcept {
        drained_snapshot_hook_.store(hook);
    }
#endif
    bool set_coordinate_seed(std::uint64_t seed) noexcept {
        return update_control([&] { coordinate_seed_.store(seed); });
    }
    int push(const SampleType* input, int count) noexcept {
        auto word = control_.load(std::memory_order_acquire);
        auto state = control_state(word);
        if ((state != State::Quiescent && state != State::Live) || input == nullptr || count <= 0)
            return 0;
        const bool starting = state == State::Quiescent;
        std::uint8_t startup_slot{};
        if (state == State::Quiescent) {
            if (!std::isfinite(input[0]))
                return 0;
            for (;;) {
                state = control_state(word);
                if (state != State::Quiescent)
                    return 0;
                startup_slot = control_slot(word);
                const auto desired =
                    pack_control(State::Pushing, startup_slot, control_configured(word));
                if (control_.compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                    break;
            }
            if (!ensure_reservation()) {
                word = control_.load(std::memory_order_acquire);
                for (;;) {
                    const auto target =
                        control_finish_requested(word) ? State::Finished : State::Quiescent;
                    const auto completed =
                        pack_control(target, control_slot(word), control_configured(word));
                    if (control_.compare_exchange_weak(word, completed, std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
                        return 0;
                }
            }
        } else {
            for (;;) {
                state = control_state(word);
                if (state != State::Live)
                    return 0;
                if (control_.compare_exchange_weak(word, with_state(word, State::Pushing),
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                    break;
            }
        }
        if (starting) {
#if defined(PULP_WAVESET_TEST_SEAMS)
            if (const auto hook = startup_hook_.load(std::memory_order_acquire))
                hook();
#endif
            apply_program(startup_slot);
        }
        int accepted = 0;
        while (accepted < count) {
            const auto sample = input[accepted];
            if (!std::isfinite(sample))
                break;
            if (capture_size_ != 0 && is_boundary(sample)) {
                finalize_capture(false);
                if (current_state() == State::Faulted)
                    break;
                if (!ensure_reservation())
                    break;
            } else if (capture_size_ == 0 && !ensure_reservation()) {
                break;
            }
            if (capture_size_ == 0)
                capture_source_begin_ = next_source_index_;
            capture_[capture_size_++] = sample;
            update_side(sample);
            ++accepted;
            ++next_source_index_;
            ++accepted_total_;
            if (capture_size_ == static_cast<std::size_t>(capacity_.max_waveset_samples))
                finalize_capture(true);
            if (current_state() == State::Faulted)
                break;
        }
        word = control_.load(std::memory_order_acquire);
        for (;;) {
            if (control_state(word) == State::Faulted)
                return accepted;
            if (control_finish_requested(word)) {
                finish_owned_processing();
                return accepted;
            }
            if (control_.compare_exchange_weak(word, with_state(word, State::Live),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                return accepted;
        }
    }
    int pull(SampleType* output, int count) noexcept {
        if (output == nullptr || count <= 0)
            return 0;
        auto word = control_.load(std::memory_order_acquire);
        State source{};
        for (;;) {
            source = control_state(word);
            if (source == State::Quiescent)
                return 0;
            if (source != State::Live && source != State::Finished)
                return 0;
            const auto owned =
                with_state(word, source == State::Finished ? State::Draining : State::Pulling);
            if (control_.compare_exchange_weak(word, owned, std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
        const auto holdback = source == State::Finished || !splicer_.valid()
                                  ? std::size_t{}
                                  : std::min(max_fade_, splicer_.full_length() / 2u);
        const auto amount = splicer_.pull(output, static_cast<std::size_t>(count), holdback);
        word = control_.load(std::memory_order_acquire);
        for (;;) {
            if (source == State::Live && control_finish_requested(word)) {
                finish_owned_processing();
                break;
            }
            if (control_.compare_exchange_weak(word, with_state(word, source),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
        return static_cast<int>(amount);
    }
    void finish_input() noexcept {
        auto word = control_.load(std::memory_order_acquire);
        for (;;) {
            const auto state = control_state(word);
            if (state == State::Unprepared || state == State::Finished ||
                state == State::Draining || state == State::Observing || state == State::Faulted ||
                state == State::Finishing || state == State::Resetting)
                return;
            if (state == State::Publishing || state == State::Pushing || state == State::Pulling ||
                state == State::Controlling) {
                const auto desired = word | kFinishRequestedMask;
                if (control_.compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                    return;
                continue;
            }
            const auto desired =
                with_state(word, state == State::Quiescent ? State::Finished : State::Finishing);
            if (!control_.compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                continue;
            if (state == State::Quiescent)
                return;
            break;
        }
#if defined(PULP_WAVESET_TEST_SEAMS)
        if (const auto hook = finish_hook_.load(std::memory_order_acquire))
            hook();
#endif
        finish_owned_processing();
    }
    bool drained() const noexcept {
        auto word = control_.load(std::memory_order_acquire);
        for (;;) {
            if (control_state(word) != State::Finished)
                return false;
            if (control_.compare_exchange_weak(word, with_state(word, State::Observing),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
#if defined(PULP_WAVESET_TEST_SEAMS)
        if (const auto hook = drained_snapshot_hook_.load(std::memory_order_acquire))
            hook();
#endif
        const bool result = splicer_.size() == 0 && capture_size_ == 0 && held_count_ == 0;
        word = control_.load(std::memory_order_acquire);
        while (!control_.compare_exchange_weak(word, with_state(word, State::Finished),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        }
        return result;
    }
    void reset() noexcept {
        auto word = control_.load(std::memory_order_acquire);
        for (;;) {
            const auto state = control_state(word);
            if (state == State::Unprepared || state == State::Publishing ||
                state == State::Pushing || state == State::Pulling || state == State::Draining ||
                state == State::Observing || state == State::Controlling ||
                state == State::Finishing || state == State::Resetting)
                return;
            if (control_.compare_exchange_weak(word, with_state(word, State::Resetting),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
#if defined(PULP_WAVESET_TEST_SEAMS)
        if (const auto hook = reset_hook_.load(std::memory_order_acquire))
            hook();
#endif
        capture_size_ = held_count_ = 0;
        ledger_.reset();
        accepted_total_ = next_source_index_ = completed_index_ = 0;
        program_cursor_ = 0;
        active_slot_ = programs_[0].configured ? 0 : first_configured_slot();
        const auto configured = control_configured(control_.load(std::memory_order_relaxed));
        segmenter_.reset();
        splicer_.reset();
#if defined(PULP_WAVESET_TEST_SEAMS)
        if (const auto hook = reset_completion_hook_.load(std::memory_order_acquire))
            hook();
#endif
        control_.store(pack_control(State::Quiescent, active_slot_, configured),
                       std::memory_order_release);
    }
    int latency_samples() const noexcept {
        return 0;
    }
    int max_lookahead_samples() const noexcept {
        return prepared() ? max_lookahead_ : 0;
    }
    std::size_t tail_samples() const noexcept {
        return prepared() ? tail_ : 0;
    }

  private:
    enum class State : std::uint8_t {
        Unprepared,
        Quiescent,
        Publishing,
        Live,
        Pushing,
        Pulling,
        Draining,
        Observing,
        Controlling,
        Finishing,
        Resetting,
        Finished,
        Faulted,
    };
    static constexpr std::uint32_t kStateShift = 3;
    static constexpr std::uint32_t kSlotMask = 0x7u;
    static constexpr std::uint32_t kConfiguredShift = 8;
    static constexpr std::uint32_t kConfiguredMask = 0xffu;
    static constexpr std::uint32_t kFinishRequestedMask = std::uint32_t{1} << 31;
    static constexpr std::uint32_t pack_control(State state, std::uint8_t slot,
                                                std::uint32_t configured = 0) noexcept {
        return (configured << kConfiguredShift) |
               (static_cast<std::uint32_t>(state) << kStateShift) | (slot & kSlotMask);
    }
    static constexpr State control_state(std::uint32_t word) noexcept {
        return static_cast<State>((word >> kStateShift) & 0x1fu);
    }
    static constexpr std::uint8_t control_slot(std::uint32_t word) noexcept {
        return static_cast<std::uint8_t>(word & kSlotMask);
    }
    static constexpr std::uint32_t control_configured(std::uint32_t word) noexcept {
        return (word >> kConfiguredShift) & kConfiguredMask;
    }
    static constexpr bool control_finish_requested(std::uint32_t word) noexcept {
        return (word & kFinishRequestedMask) != 0;
    }
    static constexpr std::uint32_t with_state(std::uint32_t word, State state) noexcept {
        return pack_control(state, control_slot(word), control_configured(word)) |
               (word & kFinishRequestedMask);
    }
    State current_state() const noexcept {
        return control_state(control_.load(std::memory_order_acquire));
    }
    void transition_terminal(State terminal) noexcept {
        auto word = control_.load(std::memory_order_acquire);
        for (;;) {
            const auto state = control_state(word);
            if (state == State::Faulted || state == State::Finished || state == State::Unprepared)
                return;
            if (state == State::Publishing)
                return;
            const auto desired =
                pack_control(terminal, control_slot(word), control_configured(word));
            if (control_.compare_exchange_weak(word, desired, std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                return;
        }
    }
    struct StoredProgram {
        std::uint32_t version = 0;
        int rotate_window = 1;
        std::size_t step_count = 0, permutation_count = 0;
        bool configured = false;
    };
    struct HeldSegment {
        std::size_t length = 0, storage_offset = 0, token_index = 0;
        std::uint64_t source_begin = 0, source_end = 0;
    };
    struct ChunkInfo {
        std::uint64_t source_begin = 0, source_end = 0;
        std::size_t length = 0;
        bool forward = true, resampled = false;
    };
    static bool checked_mul(std::size_t a, std::size_t b, std::size_t& result) noexcept {
        if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
            return false;
        result = a * b;
        return true;
    }
    void clear_configuration() noexcept {
        control_.store(pack_control(State::Unprepared, 0), std::memory_order_release);
        capture_.reset();
        scratch_.reset();
        splicer_.clear();
        rotate_storage_.reset();
        held_.reset();
        ledger_.clear();
        programs_.clear();
    }
    bool validate_program(const OperationProgram& program) const noexcept {
        if (program.version == 0 || program.steps.empty() ||
            program.steps.size() > static_cast<std::size_t>(capacity_.max_wavesets))
            return false;
        bool has_rotate = false;
        for (const auto& step : program.steps) {
            if (!valid_operation(step.operation))
                return false;
            if (step.operation == Operation::Repeat &&
                (step.repeat_count < 1 || step.repeat_count > kMaxRepeatCount))
                return false;
            if (step.operation == Operation::CoordinateSelect) {
                if (step.coordinate_choice_count < 1 ||
                    step.coordinate_choice_count > kMaxCoordinateChoices)
                    return false;
                for (std::size_t i = 0; i < step.coordinate_choice_count; ++i) {
                    const auto op = step.coordinate_choices[i];
                    if (!valid_coordinate_operation(op))
                        return false;
                }
                if (step.repeat_count < 1 || step.repeat_count > kMaxRepeatCount)
                    return false;
            }
            has_rotate = has_rotate || step.operation == Operation::Rotate;
        }
        if (!has_rotate)
            return program.permutation.empty();
        const auto w = program.rotate_window;
        if (w <= 0 || w > std::min(capacity_.max_wavesets, kMaxRotateWindow) ||
            program.permutation.size() != static_cast<std::size_t>(w))
            return false;
        std::array<bool, 256> seen{};
        for (auto value : program.permutation) {
            if (value >= static_cast<std::uint16_t>(w) || seen[value])
                return false;
            seen[value] = true;
        }
        for (std::size_t i = 0; i < program.steps.size();) {
            if (program.steps[i].operation != Operation::Rotate) {
                ++i;
                continue;
            }
            if (i + static_cast<std::size_t>(w) > program.steps.size())
                return false;
            for (int j = 0; j < w; ++j)
                if (program.steps[i + static_cast<std::size_t>(j)].operation != Operation::Rotate)
                    return false;
            i += static_cast<std::size_t>(w);
        }
        return true;
    }
    static constexpr bool valid_operation(Operation operation) noexcept {
        return operation >= Operation::Pass && operation <= Operation::CoordinateSelect;
    }
    static constexpr bool valid_coordinate_operation(Operation operation) noexcept {
        return operation == Operation::Pass || operation == Operation::Repeat ||
               operation == Operation::Omit || operation == Operation::Reverse ||
               operation == Operation::Normalize;
    }
    bool ensure_reservation() noexcept {
        if (ledger_.owns(active_token_, detail::ReservationToken::Owner::Capture))
            return true;
        std::size_t credit{};
        if (!checked_mul(ledger_.count(), max_segment_output_, credit) ||
            credit > splicer_.capacity() - splicer_.size() ||
            max_segment_output_ > splicer_.capacity() - splicer_.size() - credit)
            return false;
        const auto token = ledger_.acquire_capture();
        if (token == ledger_.capacity())
            return false;
        active_token_ = token;
        return true;
    }
    bool is_boundary(SampleType sample) const noexcept {
        return segmenter_.transition(sample, epsilon_.load(std::memory_order_relaxed),
                                     polarity_.load(std::memory_order_relaxed));
    }
    void update_side(SampleType sample) noexcept {
        segmenter_.observe(sample, epsilon_.load(std::memory_order_relaxed));
    }
    void finalize_capture(bool) noexcept {
        if (capture_size_ == 0)
            return;
        const auto& program = programs_[active_slot_];
        const auto& step = programs_.step(active_slot_, program_cursor_);
        if (step.operation == Operation::Rotate) {
            if (!ledger_.transfer_to_rotate(active_token_)) {
                fault();
                return;
            }
            auto& held = held_[held_count_];
            held.storage_offset =
                held_count_ * static_cast<std::size_t>(capacity_.max_waveset_samples);
            std::copy_n(capture_.get(), capture_size_, rotate_storage_.get() + held.storage_offset);
            held.length = capture_size_;
            held.source_begin = capture_source_begin_;
            held.source_end = capture_source_begin_ + capture_size_;
            held.token_index = active_token_;
            ++held_count_;
            if (held_count_ == static_cast<std::size_t>(program.rotate_window)) {
                for (std::size_t i = 0; i < held_count_; ++i) {
                    const auto& source = held_[programs_.permutation(active_slot_, i)];
                    if (!ledger_.owns(source.token_index,
                                      detail::ReservationToken::Owner::Rotate)) {
                        fault();
                        return;
                    }
                }
                for (std::size_t i = 0; i < held_count_; ++i) {
                    const auto& source = held_[programs_.permutation(active_slot_, i)];
                    append_chunk(
                        rotate_storage_.get() + source.storage_offset, source.length,
                        {source.source_begin, source.source_end, source.length, true, false});
                    if (!release_reservation(source.token_index,
                                             detail::ReservationToken::Owner::Rotate))
                        return;
                }
                held_count_ = 0;
                program_cursor_ =
                    (program_cursor_ + static_cast<std::size_t>(program.rotate_window)) %
                    program.step_count;
                apply_pending_program();
            }
        } else {
            if (!ledger_.owns(active_token_, detail::ReservationToken::Owner::Capture)) {
                fault();
                return;
            }
            render_step(step, capture_.get(), capture_size_, capture_source_begin_);
            if (!release_reservation(active_token_, detail::ReservationToken::Owner::Capture))
                return;
            program_cursor_ = (program_cursor_ + 1u) % program.step_count;
            apply_pending_program();
        }
        ++completed_index_;
        capture_size_ = 0;
    }
    void render_step(const ProgramStep& step, const SampleType* samples, std::size_t length,
                     std::uint64_t source_begin) noexcept {
        auto operation = step.operation;
        if (operation == Operation::CoordinateSelect) {
            const auto draw = unit_from<double>(
                rng_key(coordinate_seed_.load(std::memory_order_relaxed), completed_index_));
            const auto choice =
                std::min<std::size_t>(static_cast<std::size_t>(draw * step.coordinate_choice_count),
                                      step.coordinate_choice_count - 1u);
            operation = step.coordinate_choices[choice];
        }
        if (operation == Operation::Omit) {
            age_splice_suffix();
            return;
        }
        std::size_t result_length = length;
        bool forward = true;
        bool resampled = false;
        switch (operation) {
        case Operation::Pass:
            std::copy_n(samples, length, scratch_.get());
            break;
        case Operation::Repeat:
            for (std::size_t copy = 0; copy < step.repeat_count; ++copy)
                append_chunk(samples, length,
                             {source_begin, source_begin + length, length, true, false});
            return;
        case Operation::Reverse:
            std::reverse_copy(samples, samples + length, scratch_.get());
            forward = false;
            break;
        case Operation::Normalize: {
            const auto ratio = normalize_ratio_.load(std::memory_order_relaxed);
            result_length = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::llround(static_cast<long double>(length) * ratio)),
                1u,
                static_cast<std::size_t>(
                    std::ceil(capacity_.max_waveset_samples * capacity_.max_normalize_ratio)));
            if (result_length == 1 || length == 1) {
                std::fill_n(scratch_.get(), result_length, samples[0]);
            } else {
                for (std::size_t i = 0; i < result_length; ++i) {
                    const long double position =
                        static_cast<long double>(i) * (length - 1u) / (result_length - 1u);
                    const auto lower = static_cast<std::size_t>(position);
                    const auto upper = std::min(lower + 1u, length - 1u);
                    const auto fraction = static_cast<SampleType>(position - lower);
                    scratch_[i] = samples[lower] + (samples[upper] - samples[lower]) * fraction;
                }
            }
            resampled = result_length != length;
            break;
        }
        default:
            return;
        }
        append_chunk(scratch_.get(), result_length,
                     {source_begin, source_begin + length, result_length, forward, resampled});
    }
    void append_chunk(const SampleType* samples, std::size_t length, ChunkInfo info) noexcept {
        if (length == 0)
            return;
        const bool contiguous_identity =
            splicer_.valid() && previous_chunk_.forward && !previous_chunk_.resampled &&
            info.forward && !info.resampled && previous_chunk_.source_end == info.source_begin;
        std::size_t fade = 0;
        if (splicer_.valid() && !contiguous_identity) {
            const auto requested = std::min(
                max_fade_, static_cast<std::size_t>(std::llround(
                               crossfade_ms_.load(std::memory_order_relaxed) * SampleType{0.001} *
                               static_cast<SampleType>(sample_rate_))));
            fade = std::min({requested, splicer_.full_length() / 2u, length / 2u, splicer_.size()});
        }
        const auto law =
            static_cast<CrossfadeGainLaw>(crossfade_law_.load(std::memory_order_relaxed));
        splicer_.append(samples, length, fade, law);
        previous_chunk_ = info;
    }
    void age_splice_suffix() noexcept {
        splicer_.age(static_cast<std::size_t>(capacity_.max_wavesets));
    }
    bool release_reservation(std::size_t token, detail::ReservationToken::Owner owner) noexcept {
        if (ledger_.release(token, owner))
            return true;
        fault();
        return false;
    }
    void fault() noexcept {
        transition_terminal(State::Faulted);
    }
    void finish_owned_processing() noexcept {
        if (capture_size_ != 0)
            finalize_capture(true);
        if (current_state() == State::Faulted)
            return;
        flush_partial_rotate();
        if (current_state() != State::Faulted)
            transition_terminal(State::Finished);
    }
    void apply_pending_program() noexcept {
        apply_program(control_slot(control_.load(std::memory_order_acquire)));
    }
    void apply_program(std::uint8_t slot) noexcept {
        if (slot == active_slot_)
            return;
        active_slot_ = slot;
        program_cursor_ = 0;
    }
    void flush_partial_rotate() noexcept {
        for (std::size_t i = 0; i < held_count_; ++i) {
            if (!ledger_.owns(held_[i].token_index, detail::ReservationToken::Owner::Rotate)) {
                fault();
                return;
            }
        }
        for (std::size_t i = 0; i < held_count_; ++i) {
            const auto& segment = held_[i];
            append_chunk(rotate_storage_.get() + segment.storage_offset, segment.length,
                         {segment.source_begin, segment.source_end, segment.length, true, false});
            if (!release_reservation(segment.token_index, detail::ReservationToken::Owner::Rotate))
                return;
        }
        held_count_ = 0;
    }
    std::uint8_t first_configured_slot() const noexcept {
        for (std::uint8_t i = 0; i < kMaxProgramSlots; ++i)
            if (programs_[i].configured)
                return i;
        return 0;
    }
    template <typename Mutation> bool update_control(Mutation&& mutation) noexcept {
        auto word = control_.load(std::memory_order_acquire);
        State source{};
        for (;;) {
            source = control_state(word);
            if (control_finish_requested(word) ||
                (source != State::Quiescent && source != State::Live))
                return false;
            if (control_.compare_exchange_weak(word, with_state(word, State::Controlling),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
        mutation();
#if defined(PULP_WAVESET_TEST_SEAMS)
        if (const auto hook = control_completion_hook_.load(std::memory_order_acquire))
            hook();
#endif
        word = control_.load(std::memory_order_acquire);
        for (;;) {
            if (control_finish_requested(word)) {
                finish_owned_processing();
                return true;
            }
            if (control_.compare_exchange_weak(word, with_state(word, source),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                return true;
        }
    }
    detail::ProgramBank<StoredProgram, ProgramStep, kMaxProgramSlots> programs_{};
    std::unique_ptr<SampleType[]> capture_, scratch_, rotate_storage_;
    std::unique_ptr<HeldSegment[]> held_;
    detail::ReservationLedger ledger_;
    detail::Segmenter<SampleType> segmenter_;
    detail::OutputSplicer<SampleType> splicer_;
    Capacity capacity_{};
    double sample_rate_ = 0.0, max_normalize_ratio_ = 1.0;
    std::size_t max_segment_output_ = 0, max_fade_ = 0;
    int max_lookahead_ = 0;
    std::size_t tail_ = 0, capture_size_ = 0, held_count_ = 0;
    std::size_t active_token_ = 0, program_cursor_ = 0;
    std::uint64_t capture_source_begin_ = 0, next_source_index_ = 0;
    std::uint64_t accepted_total_ = 0, completed_index_ = 0;
    ChunkInfo previous_chunk_{};
    std::uint8_t active_slot_ = 0;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    mutable std::atomic<std::uint32_t> control_{pack_control(State::Unprepared, 0)};
    std::atomic<std::uint8_t> polarity_{static_cast<std::uint8_t>(ZeroCrossingPolarity::Rising)};
    std::atomic<SampleType> epsilon_{SampleType{}};
    std::atomic<std::uint8_t> crossfade_law_{
        static_cast<std::uint8_t>(CrossfadeGainLaw::EqualGain)};
    std::atomic<SampleType> crossfade_ms_{SampleType{}}, normalize_ratio_{SampleType{1}};
    std::atomic<std::uint64_t> coordinate_seed_{0};
    static_assert(std::atomic<SampleType>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
#if defined(PULP_WAVESET_TEST_SEAMS)
    inline static std::atomic<StartupHook> startup_hook_{nullptr};
    inline static std::atomic<PublicationHook> publication_hook_{nullptr};
    inline static std::atomic<FinishHook> finish_hook_{nullptr};
    inline static std::atomic<ResetHook> reset_hook_{nullptr};
    inline static std::atomic<ResetCompletionHook> reset_completion_hook_{nullptr};
    inline static std::atomic<ControlCompletionHook> control_completion_hook_{nullptr};
    inline static std::atomic<DrainedSnapshotHook> drained_snapshot_hook_{nullptr};
#endif
};
using WavesetTransformer = WavesetTransformerT<float>;
using WavesetTransformer64 = WavesetTransformerT<double>;
} // namespace pulp::signal
