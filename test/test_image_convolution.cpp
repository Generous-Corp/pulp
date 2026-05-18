#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/image_convolution.hpp>
#include <vector>
#include <numeric>

using namespace pulp::canvas;

// Helper: create a test image (RGBA, solid color)
static std::vector<uint8_t> make_solid_image(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 4));
    for (int i = 0; i < w * h; ++i) {
        pixels[static_cast<size_t>(i * 4)] = r;
        pixels[static_cast<size_t>(i * 4 + 1)] = g;
        pixels[static_cast<size_t>(i * 4 + 2)] = b;
        pixels[static_cast<size_t>(i * 4 + 3)] = 255;
    }
    return pixels;
}

TEST_CASE("ImageConvolutionKernel construction", "[canvas][convolution]") {
    ImageConvolutionKernel k(3);
    REQUIRE(k.size() == 3);

    // Even size forced to odd
    ImageConvolutionKernel k2(4);
    REQUIRE(k2.size() == 5);

    // Size 0 forced to 1
    ImageConvolutionKernel k3(0);
    REQUIRE(k3.size() == 1);

    ImageConvolutionKernel k4(-7);
    REQUIRE(k4.size() == 1);
}

TEST_CASE("ImageConvolutionKernel set and get", "[canvas][convolution]") {
    ImageConvolutionKernel k(3);
    k.set(0, 0, 1.0f);
    k.set(1, 1, 5.0f);
    k.set(2, 2, -1.0f);

    REQUIRE(k.get(0, 0) == 1.0f);
    REQUIRE(k.get(1, 1) == 5.0f);
    REQUIRE(k.get(2, 2) == -1.0f);
    REQUIRE(k.get(0, 1) == 0.0f);  // default
}

TEST_CASE("Identity kernel preserves image", "[canvas][convolution]") {
    // Identity kernel: 0 0 0 / 0 1 0 / 0 0 0
    ImageConvolutionKernel identity(3);
    identity.set(1, 1, 1.0f);

    auto pixels = make_solid_image(4, 4, 128, 64, 32);
    auto original = pixels;

    identity.apply(pixels.data(), 4, 4);

    // All pixels should be unchanged
    for (size_t i = 0; i < pixels.size(); ++i) {
        REQUIRE(pixels[i] == original[i]);
    }
}

TEST_CASE("Identity kernel supports explicit row stride", "[canvas][convolution][issue-641]") {
    ImageConvolutionKernel identity(3);
    identity.set(1, 1, 1.0f);

    constexpr int w = 2;
    constexpr int h = 2;
    constexpr int stride = w * 4;
    std::vector<uint8_t> pixels = {
        10, 20, 30, 40, 50, 60, 70, 80,
        90, 100, 110, 120, 130, 140, 150, 160,
    };
    const auto original = pixels;

    identity.apply(pixels.data(), w, h, stride);
    REQUIRE(pixels == original);
}

TEST_CASE("ImageConvolutionKernel clamps color output", "[canvas][convolution][issue-641]") {
    ImageConvolutionKernel boost(1);
    boost.set(0, 0, 3.0f);
    uint8_t bright[4] = {100, 120, 200, 77};
    boost.apply(bright, 1, 1);
    REQUIRE(bright[0] == 255);
    REQUIRE(bright[1] == 255);
    REQUIRE(bright[2] == 255);
    REQUIRE(bright[3] == 77);

    ImageConvolutionKernel invert(1);
    invert.set(0, 0, -1.0f);
    uint8_t dark[4] = {100, 120, 200, 88};
    invert.apply(dark, 1, 1);
    REQUIRE(dark[0] == 0);
    REQUIRE(dark[1] == 0);
    REQUIRE(dark[2] == 0);
    REQUIRE(dark[3] == 88);
}

TEST_CASE("Gaussian blur smooths uniform image", "[canvas][convolution]") {
    // A uniform image should be unchanged by blur (all neighbors are the same)
    auto k = ImageConvolutionKernel::gaussian_blur_3();
    auto pixels = make_solid_image(8, 8, 100, 100, 100);
    auto original = pixels;

    k.apply(pixels.data(), 8, 8);

    // Center pixels should be nearly unchanged (edges may vary due to clamping)
    int center = 4 * 8 + 4;  // row 4, col 4
    int idx = center * 4;
    REQUIRE(std::abs(static_cast<int>(pixels[idx]) - 100) <= 1);
}

TEST_CASE("Blur reduces contrast of edge", "[canvas][convolution]") {
    // Create image with sharp edge: left half white, right half black
    int w = 16, h = 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 4));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = (x < w / 2) ? 255 : 0;
            int idx = (y * w + x) * 4;
            pixels[idx] = v; pixels[idx + 1] = v; pixels[idx + 2] = v; pixels[idx + 3] = 255;
        }
    }

    auto k = ImageConvolutionKernel::gaussian_blur_3();
    k.apply(pixels.data(), w, h);

    // Pixel at the edge (x=7, y=2) should be intermediate (not 255 or 0)
    int edge_idx = (2 * w + 7) * 4;
    REQUIRE(pixels[edge_idx] > 0);
    REQUIRE(pixels[edge_idx] < 255);
}

TEST_CASE("Apply on null/zero returns safely", "[canvas][convolution]") {
    auto k = ImageConvolutionKernel::gaussian_blur_3();
    k.apply(nullptr, 10, 10);  // should not crash
    uint8_t dummy[4] = {128, 128, 128, 255};
    k.apply(dummy, 0, 0);      // zero dimensions
    k.apply(dummy, -1, 10);    // negative dimensions
}

TEST_CASE("Alpha channel preserved", "[canvas][convolution]") {
    int w = 4, h = 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 4));
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4] = 100;
        pixels[i * 4 + 1] = 100;
        pixels[i * 4 + 2] = 100;
        pixels[i * 4 + 3] = 200;  // non-trivial alpha
    }

    auto k = ImageConvolutionKernel::gaussian_blur_3();
    k.apply(pixels.data(), w, h);

    // Alpha should be preserved (not convolved)
    for (int i = 0; i < w * h; ++i) {
        REQUIRE(pixels[i * 4 + 3] == 200);
    }
}

TEST_CASE("Standard kernels have correct size", "[canvas][convolution]") {
    REQUIRE(ImageConvolutionKernel::gaussian_blur_3().size() == 3);
    REQUIRE(ImageConvolutionKernel::gaussian_blur_5().size() == 5);
    REQUIRE(ImageConvolutionKernel::sharpen().size() == 3);
    REQUIRE(ImageConvolutionKernel::edge_detect().size() == 3);
    REQUIRE(ImageConvolutionKernel::emboss().size() == 3);
}

TEST_CASE("Sharpen kernel center value is positive", "[canvas][convolution]") {
    auto k = ImageConvolutionKernel::sharpen();
    REQUIRE(k.get(1, 1) > 0.0f);  // center should amplify
}

TEST_CASE("Standard kernel weights cover blur edge and emboss paths",
          "[canvas][convolution][issue-641]") {
    auto blur5 = ImageConvolutionKernel::gaussian_blur_5();
    float blur_sum = 0.0f;
    for (int row = 0; row < blur5.size(); ++row) {
        for (int col = 0; col < blur5.size(); ++col)
            blur_sum += blur5.get(row, col);
    }
    REQUIRE(std::abs(blur_sum - 1.0f) < 0.0001f);

    auto edge = ImageConvolutionKernel::edge_detect();
    REQUIRE(edge.get(1, 1) == 8.0f);
    REQUIRE(edge.get(0, 0) == -1.0f);

    auto emboss = ImageConvolutionKernel::emboss();
    REQUIRE(emboss.get(0, 0) == -2.0f);
    REQUIRE(emboss.get(2, 2) == 2.0f);
}

TEST_CASE("Standard kernels process a single pixel via edge clamping",
          "[canvas][convolution][coverage][phase3-github]") {
    auto pixel = make_solid_image(1, 1, 20, 40, 60);
    ImageConvolutionKernel::gaussian_blur_3().apply(pixel.data(), 1, 1);
    REQUIRE(pixel[0] == 20);
    REQUIRE(pixel[1] == 40);
    REQUIRE(pixel[2] == 60);
    REQUIRE(pixel[3] == 255);

    ImageConvolutionKernel::sharpen().apply(pixel.data(), 1, 1);
    REQUIRE(pixel[0] == 20);
    REQUIRE(pixel[1] == 40);
    REQUIRE(pixel[2] == 60);
    REQUIRE(pixel[3] == 255);
}

TEST_CASE("Single-pixel convolution clamps repeated edge samples",
          "[canvas][convolution][coverage][phase3-large]") {
    uint8_t sharpened[4] = {40, 80, 120, 77};
    auto sharpen = ImageConvolutionKernel::sharpen();
    sharpen.apply(sharpened, 1, 1);
    REQUIRE(sharpened[0] == 40);
    REQUIRE(sharpened[1] == 80);
    REQUIRE(sharpened[2] == 120);
    REQUIRE(sharpened[3] == 77);

    uint8_t edge[4] = {40, 80, 120, 88};
    auto edge_detect = ImageConvolutionKernel::edge_detect();
    edge_detect.apply(edge, 1, 1);
    REQUIRE(edge[0] == 0);
    REQUIRE(edge[1] == 0);
    REQUIRE(edge[2] == 0);
    REQUIRE(edge[3] == 88);
}
