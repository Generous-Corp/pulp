#pragma once

#include <vector>
#include <cmath>

namespace pulp::signal {

// Window functions for FFT, spectral analysis, FIR filter design.
//
// RT contract: generate() allocates and is prepare/design-time only. apply()
// is allocation-free when callers pass a valid buffer and a precomputed window.
class WindowFunction {
public:
    enum class Type { rectangular, hann, hamming, blackman, flat_top, kaiser };

    // Generate a window of the given size and type. Not real-time safe.
    template <typename SampleType = float>
    static std::vector<SampleType> generate(int size,
                                            Type type,
                                            SampleType param = SampleType{0.0f}) {
        if (size <= 0) return {};
        if (size == 1) return {SampleType{1.0f}};

        std::vector<SampleType> w(size);
        for (int i = 0; i < size; ++i) {
            SampleType n = static_cast<SampleType>(i);
            SampleType N = static_cast<SampleType>(size - 1);

            switch (type) {
                case Type::rectangular:
                    w[i] = SampleType{1.0f};
                    break;

                case Type::hann:
                    w[i] = SampleType{0.5f} *
                           (SampleType{1.0f} - std::cos(SampleType{2.0f} *
                                                        pi<SampleType> * n / N));
                    break;

                case Type::hamming:
                    w[i] = SampleType{0.54f} -
                           SampleType{0.46f} *
                               std::cos(SampleType{2.0f} * pi<SampleType> * n / N);
                    break;

                case Type::blackman:
                    w[i] = SampleType{0.42f} -
                           SampleType{0.5f} *
                               std::cos(SampleType{2.0f} * pi<SampleType> * n / N)
                           + SampleType{0.08f} *
                                 std::cos(SampleType{4.0f} * pi<SampleType> * n / N);
                    break;

                case Type::flat_top:
                    w[i] = SampleType{0.21557895f}
                           - SampleType{0.41663158f} *
                                 std::cos(SampleType{2.0f} * pi<SampleType> * n / N)
                           + SampleType{0.277263158f} *
                                 std::cos(SampleType{4.0f} * pi<SampleType> * n / N)
                           - SampleType{0.083578947f} *
                                 std::cos(SampleType{6.0f} * pi<SampleType> * n / N)
                           + SampleType{0.006947368f} *
                                 std::cos(SampleType{8.0f} * pi<SampleType> * n / N);
                    break;

                case Type::kaiser: {
                    SampleType alpha = param > SampleType{0.0f}
                        ? param
                        : SampleType{3.0f};
                    SampleType x = SampleType{2.0f} * n / N - SampleType{1.0f};
                    w[i] = bessel_i0(alpha * std::sqrt(SampleType{1.0f} - x * x)) /
                           bessel_i0(alpha);
                    break;
                }
            }
        }
        return w;
    }

    // Apply a precomputed window to a buffer in-place. Allocation-free.
    template <typename SampleType>
    static void apply(SampleType* buffer, const std::vector<SampleType>& window) {
        for (size_t i = 0; i < window.size(); ++i)
            buffer[i] *= window[i];
    }

private:
    template <typename SampleType>
    static constexpr SampleType pi =
        static_cast<SampleType>(3.141592653589793238462643383279502884L);

    // Modified Bessel function of the first kind, order 0 (for Kaiser window)
    template <typename SampleType>
    static SampleType bessel_i0(SampleType x) {
        SampleType sum = SampleType{1.0f};
        SampleType term = SampleType{1.0f};
        SampleType x2 = x * x * SampleType{0.25f};
        for (int k = 1; k < 20; ++k) {
            term *= x2 / (static_cast<SampleType>(k) * static_cast<SampleType>(k));
            sum += term;
        }
        return sum;
    }
};

} // namespace pulp::signal
