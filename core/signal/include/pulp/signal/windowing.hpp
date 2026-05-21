#pragma once

#include <vector>
#include <cmath>

namespace pulp::signal {

// Window functions for FFT, spectral analysis, FIR filter design
class WindowFunction {
public:
    enum class Type { rectangular, hann, hamming, blackman, flat_top, kaiser };

    // Generate a window of the given size and type
    static std::vector<float> generate(int size, Type type, float param = 0.0f) {
        if (size <= 0) return {};
        if (size == 1) return {1.0f};

        std::vector<float> w(size);
        for (int i = 0; i < size; ++i) {
            float n = static_cast<float>(i);
            float N = static_cast<float>(size - 1);

            switch (type) {
                case Type::rectangular:
                    w[i] = 1.0f;
                    break;

                case Type::hann:
                    w[i] = 0.5f * (1.0f - std::cos(2.0f * pi * n / N));
                    break;

                case Type::hamming:
                    w[i] = 0.54f - 0.46f * std::cos(2.0f * pi * n / N);
                    break;

                case Type::blackman:
                    w[i] = 0.42f - 0.5f * std::cos(2.0f * pi * n / N)
                           + 0.08f * std::cos(4.0f * pi * n / N);
                    break;

                case Type::flat_top:
                    w[i] = 0.21557895f
                           - 0.41663158f * std::cos(2.0f * pi * n / N)
                           + 0.277263158f * std::cos(4.0f * pi * n / N)
                           - 0.083578947f * std::cos(6.0f * pi * n / N)
                           + 0.006947368f * std::cos(8.0f * pi * n / N);
                    break;

                case Type::kaiser: {
                    float alpha = param > 0 ? param : 3.0f;
                    float x = 2.0f * n / N - 1.0f;
                    w[i] = bessel_i0(alpha * std::sqrt(1.0f - x * x)) / bessel_i0(alpha);
                    break;
                }
            }
        }
        return w;
    }

    // Apply a window to a buffer in-place
    static void apply(float* buffer, const std::vector<float>& window) {
        for (size_t i = 0; i < window.size(); ++i)
            buffer[i] *= window[i];
    }

private:
    static constexpr float pi = 3.14159265358979323846f;

    // Modified Bessel function of the first kind, order 0 (for Kaiser window)
    static float bessel_i0(float x) {
        float sum = 1.0f;
        float term = 1.0f;
        float x2 = x * x * 0.25f;
        for (int k = 1; k < 20; ++k) {
            term *= x2 / (static_cast<float>(k) * static_cast<float>(k));
            sum += term;
        }
        return sum;
    }
};

} // namespace pulp::signal
