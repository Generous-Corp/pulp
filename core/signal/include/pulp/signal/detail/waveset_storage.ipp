#pragma once

#include "pulp/signal/crossfade.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace pulp::signal::detail {
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

} // namespace pulp::signal::detail
