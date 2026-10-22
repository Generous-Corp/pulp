#pragma once

/// @file sos_cascade.hpp
/// Fixed-capacity runtime executor for cascades of second-order IIR sections.

#include <pulp/signal/biquad.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class SosCascadeInstallStatus {
    installed,
    not_prepared,
    over_capacity,
    non_finite,
    unstable,
};

/// Coefficient changes take effect immediately. The policy says what happens
/// to the recursive DF2T tail at that boundary; it does not crossfade designs.
enum class SosCascadeTransition {
    reset_state,
    preserve_state,
};

/// Executes a bounded cascade of normalized second-order sections (SOS).
///
/// This class deliberately accepts only BiquadCoefficientsT sections. It is not
/// an arbitrary-order direct-form IIR implementation. Call prepare() on a
/// control thread, then install a complete coefficient span at a block boundary.
/// Validation is whole-cascade transactional: a rejected span leaves both the
/// active coefficients and every recursive state unchanged.
///
/// RT contract: storage is a fixed std::array. process(), reset(), prepare(),
/// and coefficient installation perform bounded work and allocate no memory.
template <typename SampleType = float, std::size_t MaxSections = 16> class SosCascadeT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "SosCascadeT requires a floating-point sample type");
    static_assert(MaxSections > 0, "SosCascadeT requires non-zero storage");

  public:
    using Coefficients = BiquadCoefficientsT<SampleType>;

    /// Select the usable prefix of fixed storage and clear the active cascade.
    /// Invalid capacities are rejected without changing the object.
    bool prepare(std::size_t section_capacity) noexcept {
        if (section_capacity == 0 || section_capacity > MaxSections)
            return false;
        for (auto& section : sections_) {
            section.set_coefficients(Coefficients{});
            section.reset();
        }
        capacity_ = section_capacity;
        size_ = 0;
        return true;
    }

    bool prepared() const noexcept {
        return capacity_ != 0;
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }
    std::size_t size() const noexcept {
        return size_;
    }
    static constexpr std::size_t storage_capacity() noexcept {
        return MaxSections;
    }

    /// Install one complete SOS cascade. An empty span is a valid bypass.
    ///
    /// With preserve_state, sections that remain at the same ordinal keep their
    /// recursive tail; newly activated and removed sections are cleared. With
    /// reset_state, every section is cleared after the coefficients commit.
    template <typename CoefficientType, std::size_t Extent>
    SosCascadeInstallStatus
    set_coefficients(std::span<BiquadCoefficientsT<CoefficientType>, Extent> coefficients,
                     SosCascadeTransition transition = SosCascadeTransition::reset_state) noexcept {
        return install<CoefficientType>(
            std::span<const BiquadCoefficientsT<CoefficientType>>(coefficients), transition);
    }

    template <typename CoefficientType, std::size_t Extent>
    SosCascadeInstallStatus
    set_coefficients(std::span<const BiquadCoefficientsT<CoefficientType>, Extent> coefficients,
                     SosCascadeTransition transition = SosCascadeTransition::reset_state) noexcept {
        return install<CoefficientType>(coefficients, transition);
    }

    SampleType process(SampleType input) noexcept {
        for (std::size_t i = 0; i < size_; ++i)
            input = sections_[i].process(input);
        return input;
    }

    /// Process a buffer in place.
    ///
    /// Sample-major over up to kSectionsPerPass sections at a time: each
    /// sample runs through every section of the pass before the next sample
    /// starts, so the sections' recursions are independent dependency chains
    /// that execute concurrently instead of one section's whole buffer waiting
    /// on its own feedback latency. Every section uses BiquadT's expressions
    /// on register-resident coefficients and state, and the denormal snap runs
    /// every BiquadT::kStateSnapInterval samples and at the end of the call,
    /// so the output is identical to the sample path while the states are at
    /// or above 1e-15 (see BiquadT::process(SampleType*, int)).
    void process(SampleType* buffer, std::size_t num_samples) noexcept {
        std::size_t first = 0;
        while (first < size_) {
            const std::size_t count = size_ - first < kSectionsPerPass ? size_ - first
                                                                       : kSectionsPerPass;
            switch (count) {
                case 1: process_pass<1>(first, buffer, num_samples); break;
                case 2: process_pass<2>(first, buffer, num_samples); break;
                case 3: process_pass<3>(first, buffer, num_samples); break;
                case 4: process_pass<4>(first, buffer, num_samples); break;
                case 5: process_pass<5>(first, buffer, num_samples); break;
                case 6: process_pass<6>(first, buffer, num_samples); break;
                case 7: process_pass<7>(first, buffer, num_samples); break;
                default: process_pass<8>(first, buffer, num_samples); break;
            }
            first += count;
        }
    }

    /// Sections the block path runs together in one sample-major pass. Eight
    /// sections' state fits in registers alongside the working set.
    static constexpr std::size_t kSectionsPerPass = 8;

    void reset() noexcept {
        for (std::size_t i = 0; i < MaxSections; ++i)
            sections_[i].reset();
    }

    Coefficients coefficients(std::size_t index) const noexcept {
        return index < size_ ? sections_[index].coefficients() : Coefficients{};
    }

  private:
    template <std::size_t Count>
    void process_pass(std::size_t first, SampleType* buffer, std::size_t num_samples) noexcept {
        constexpr std::size_t kSnap = static_cast<std::size_t>(BiquadT<SampleType>::kStateSnapInterval);
        SampleType b0[Count], b1[Count], b2[Count], a1[Count], a2[Count], s1[Count], s2[Count];
        for (std::size_t k = 0; k < Count; ++k) {
            const auto& section = sections_[first + k];
            b0[k] = section.b0_;
            b1[k] = section.b1_;
            b2[k] = section.b2_;
            a1[k] = section.a1_;
            a2[k] = section.a2_;
            s1[k] = section.s1_;
            s2[k] = section.s2_;
        }
        for (std::size_t start = 0; start < num_samples; start += kSnap) {
            const std::size_t end = num_samples - start < kSnap ? num_samples : start + kSnap;
            for (std::size_t i = start; i < end; ++i) {
                SampleType x = buffer[i];
                for (std::size_t k = 0; k < Count; ++k) {
                    const SampleType output = b0[k] * x + s1[k];
                    s1[k] = b1[k] * x - a1[k] * output + s2[k];
                    s2[k] = b2[k] * x - a2[k] * output;
                    x = output;
                }
                buffer[i] = x;
            }
            for (std::size_t k = 0; k < Count; ++k) {
                s1[k] = snap_to_zero(s1[k]);
                s2[k] = snap_to_zero(s2[k]);
            }
        }
        for (std::size_t k = 0; k < Count; ++k) {
            sections_[first + k].s1_ = s1[k];
            sections_[first + k].s2_ = s2[k];
        }
    }

    template <typename CoefficientType>
    SosCascadeInstallStatus
    install(std::span<const BiquadCoefficientsT<CoefficientType>> coefficients,
            SosCascadeTransition transition) noexcept {
        static_assert(std::is_floating_point_v<CoefficientType>);
        if (!prepared())
            return SosCascadeInstallStatus::not_prepared;
        if (coefficients.size() > capacity_)
            return SosCascadeInstallStatus::over_capacity;

        for (std::size_t i = 0; i < coefficients.size(); ++i) {
            const auto& source = coefficients[i];
            const Coefficients candidate{
                static_cast<SampleType>(source.b0), static_cast<SampleType>(source.b1),
                static_cast<SampleType>(source.b2), static_cast<SampleType>(source.a1),
                static_cast<SampleType>(source.a2)};
            if (!finite(candidate))
                return SosCascadeInstallStatus::non_finite;
            if (!biquad_is_stable(candidate))
                return SosCascadeInstallStatus::unstable;
        }

        const std::size_t old_size = size_;
        for (std::size_t i = 0; i < coefficients.size(); ++i)
            sections_[i].set_coefficients(coefficients[i]);
        size_ = coefficients.size();

        if (transition == SosCascadeTransition::reset_state) {
            reset();
        } else {
            for (std::size_t i = old_size; i < size_; ++i)
                sections_[i].reset();
            for (std::size_t i = size_; i < old_size; ++i)
                sections_[i].reset();
        }
        return SosCascadeInstallStatus::installed;
    }
    static bool finite(const Coefficients& c) noexcept {
        return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
               std::isfinite(c.a1) && std::isfinite(c.a2);
    }

    std::array<BiquadT<SampleType>, MaxSections> sections_{};
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

using SosCascade = SosCascadeT<float>;
using SosCascade64 = SosCascadeT<double>;

} // namespace pulp::signal
