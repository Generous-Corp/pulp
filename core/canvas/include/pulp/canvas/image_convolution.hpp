#pragma once

// ImageConvolutionKernel — NxN kernel convolution on image data.
// Supports blur, sharpen, edge detect, and custom kernels.

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace pulp::canvas {

/// NxN convolution kernel for image processing
class ImageConvolutionKernel {
public:
    /// Create an NxN kernel (must be odd: 3, 5, 7, etc.)
    explicit ImageConvolutionKernel(int size) : size_(size) {
        if (size < 1) size_ = 1;
        if (size_ % 2 == 0) size_ += 1;  // Force odd
        values_.resize(static_cast<size_t>(size_ * size_), 0.0f);
    }

    /// Set a kernel value at (row, col)
    void set(int row, int col, float value) {
        values_[static_cast<size_t>(row * size_ + col)] = value;
    }

    /// Get a kernel value
    float get(int row, int col) const {
        return values_[static_cast<size_t>(row * size_ + col)];
    }

    /// Kernel size
    int size() const { return size_; }

    /// Apply the kernel to RGBA image data (in-place)
    void apply(uint8_t* pixels, int width, int height, int stride = 0) const {
        if (!pixels || width <= 0 || height <= 0 || size_ < 1) return;
        if (stride == 0) stride = width * 4;
        int half = size_ / 2;

        std::vector<uint8_t> temp(pixels, pixels + static_cast<size_t>(height * stride));

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float r = 0, g = 0, b = 0;

                for (int ky = -half; ky <= half; ++ky) {
                    for (int kx = -half; kx <= half; ++kx) {
                        int sy = std::clamp(y + ky, 0, height - 1);
                        int sx = std::clamp(x + kx, 0, width - 1);
                        float k = values_[static_cast<size_t>((ky + half) * size_ + (kx + half))];

                        int idx = sy * stride + sx * 4;
                        r += static_cast<float>(pixels[idx]) * k;
                        g += static_cast<float>(pixels[idx + 1]) * k;
                        b += static_cast<float>(pixels[idx + 2]) * k;
                    }
                }

                int out_idx = y * stride + x * 4;
                temp[out_idx] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                temp[out_idx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                temp[out_idx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                temp[out_idx + 3] = pixels[out_idx + 3];  // Preserve alpha
            }
        }

        std::copy(temp.begin(), temp.end(), pixels);
    }

    // ── Standard kernels ────────────────────────────────────────────────

    /// Gaussian blur (3x3)
    static ImageConvolutionKernel gaussian_blur_3() {
        ImageConvolutionKernel k(3);
        float v[] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
        for (int i = 0; i < 9; ++i) k.values_[i] = v[i] / 16.0f;
        return k;
    }

    /// Gaussian blur (5x5)
    static ImageConvolutionKernel gaussian_blur_5() {
        ImageConvolutionKernel k(5);
        float v[] = {
            1,  4,  6,  4, 1,
            4, 16, 24, 16, 4,
            6, 24, 36, 24, 6,
            4, 16, 24, 16, 4,
            1,  4,  6,  4, 1
        };
        for (int i = 0; i < 25; ++i) k.values_[i] = v[i] / 256.0f;
        return k;
    }

    /// Sharpen kernel (3x3)
    static ImageConvolutionKernel sharpen() {
        ImageConvolutionKernel k(3);
        float v[] = {0, -1, 0, -1, 5, -1, 0, -1, 0};
        for (int i = 0; i < 9; ++i) k.values_[i] = v[i];
        return k;
    }

    /// Edge detection (Sobel-like, 3x3)
    static ImageConvolutionKernel edge_detect() {
        ImageConvolutionKernel k(3);
        float v[] = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
        for (int i = 0; i < 9; ++i) k.values_[i] = v[i];
        return k;
    }

    /// Emboss (3x3)
    static ImageConvolutionKernel emboss() {
        ImageConvolutionKernel k(3);
        float v[] = {-2, -1, 0, -1, 1, 1, 0, 1, 2};
        for (int i = 0; i < 9; ++i) k.values_[i] = v[i];
        return k;
    }

private:
    int size_;
    std::vector<float> values_;
};

}  // namespace pulp::canvas
