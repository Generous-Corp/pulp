#pragma once

/// @file processor_chain.hpp
/// Variadic template composition utility for chaining DSP processors.

#include <tuple>
#include <cstddef>

namespace pulp::signal {

/// Chain multiple DSP processors in series. Each processor must have
/// a `float process(float)` method.
///
/// RT contract: `ProcessorChain` owns processors in a fixed tuple. Its
/// process/reset paths allocate no memory when the contained processors do not.
///
/// @code
/// ProcessorChain<Gain, Biquad, Compressor> chain;
/// auto& gain = chain.get<0>();
/// auto& eq = chain.get<1>();
/// auto& comp = chain.get<2>();
/// gain.set_gain_db(-3.0f);
/// float out = chain.process(in);
/// @endcode
template<typename SampleType, typename... Processors>
class ProcessorChainT {
public:
    ProcessorChainT() = default;

    /// Access a processor by index.
    template<std::size_t Index>
    auto& get() { return std::get<Index>(processors_); }

    template<std::size_t Index>
    const auto& get() const { return std::get<Index>(processors_); }

    /// Process a single sample through the entire chain.
    SampleType process(SampleType input) {
        return process_impl(input, std::index_sequence_for<Processors...>{});
    }

    /// Process a buffer in-place through the entire chain.
    void process(SampleType* data, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            data[i] = process(data[i]);
        }
    }

    /// Reset all processors in the chain.
    void reset() {
        reset_impl(std::index_sequence_for<Processors...>{});
    }

    /// Number of processors in the chain.
    static constexpr std::size_t size() { return sizeof...(Processors); }

private:
    std::tuple<Processors...> processors_;

    template<std::size_t... Is>
    SampleType process_impl(SampleType input, std::index_sequence<Is...>) {
        SampleType value = input;
        ((value = std::get<Is>(processors_).process(value)), ...);
        return value;
    }

    template<std::size_t I>
    void reset_one() {
        if constexpr (requires { std::get<I>(processors_).reset(); }) {
            std::get<I>(processors_).reset();
        }
    }

    template<std::size_t... Is>
    void reset_impl(std::index_sequence<Is...>) {
        (reset_one<Is>(), ...);
    }
};

template<typename... Processors>
using ProcessorChain = ProcessorChainT<float, Processors...>;

template<typename... Processors>
using ProcessorChain64 = ProcessorChainT<double, Processors...>;

} // namespace pulp::signal
