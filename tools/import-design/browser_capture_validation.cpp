#include "browser_capture_validation.hpp"

#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace pulp::import_design {

namespace {

bool write_bytes_atomically(
    const std::filesystem::path& destination,
    const std::vector<std::uint8_t>& bytes,
    std::string& error) {
    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(
            destination.parent_path(), ec);
    }
    if (ec) {
        error = "could not create validation artifact directory " +
                destination.parent_path().string() + ": " + ec.message();
        return false;
    }
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    auto staged = destination;
    staged += ".tmp-" + std::to_string(nonce);
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            error = "could not write validation artifact " +
                    destination.string();
            std::error_code ec;
            std::filesystem::remove(staged, ec);
            return false;
        }
    }
    ec.clear();
    std::filesystem::rename(staged, destination, ec);
    if (ec) {
        std::filesystem::remove(staged, ec);
        error = "could not commit validation artifact " +
                destination.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

BrowserCaptureValidationResult validate_browser_capture_design_ir(
    const pulp::view::DesignIR& ir,
    const BrowserCaptureValidationOptions& options) {
    BrowserCaptureValidationResult result;
    if (options.width <= 0 || options.height <= 0 ||
        options.reference.empty() || options.rendered.empty()) {
        result.error = "browser capture validation options are incomplete";
        return result;
    }

    auto root = pulp::view::build_native_view_tree(ir, ir.asset_manifest);
    if (!root) {
        result.error = "could not materialize browser capture DesignIR";
        return result;
    }
    auto rendered = pulp::view::render_to_png(
        *root, static_cast<std::uint32_t>(options.width),
        static_cast<std::uint32_t>(options.height), 2.0f, options.backend);
    if (rendered.empty()) {
        result.error = "DesignIR/Skia render failed";
        return result;
    }
    if (!write_bytes_atomically(options.rendered, rendered, result.error))
        return result;

    const auto comparison = pulp::view::compare_screenshot_files(
        options.reference.string(), options.rendered.string());
    if (!comparison.valid) {
        result.error = comparison.error;
        return result;
    }
    result.similarity = comparison.similarity;
    result.diff_pixels = comparison.diff_pixels;
    result.total_pixels = comparison.total_pixels;
    result.mean_error = comparison.mean_error;
    const float gate = options.fail_below_percent >= 0.0f
        ? options.fail_below_percent / 100.0f
        : pulp::view::kDefaultSimilarityThreshold;
    result.passes = comparison.passes(gate);

    if (!options.diff.empty()) {
        std::ifstream input(options.reference, std::ios::binary);
        if (!input) {
            result.error = "could not read browser validation reference";
            return result;
        }
        const std::vector<std::uint8_t> reference{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const auto diff = pulp::view::generate_diff_image(reference, rendered);
        if (diff.empty()) {
            result.error = "could not generate browser validation diff";
            return result;
        }
        if (!write_bytes_atomically(options.diff, diff, result.error))
            return result;
    }
    result.valid = true;
    return result;
}

}  // namespace pulp::import_design
