#include <pulp_tooling/gpu_probe/dpr_measurement.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace probe = pulp::tooling::gpu_probe;

namespace {

constexpr std::uintmax_t kMaximumRequestBytes = 1024 * 1024;

bool regular_bounded_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status) &&
        !std::filesystem::is_symlink(status) &&
        std::filesystem::file_size(path, error) <= kMaximumRequestBytes && !error;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path request_path;
    std::filesystem::path receipt_path;
    std::filesystem::path first_frame_trial_path;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--request" && index + 1 < argc)
            request_path = argv[++index];
        else if (argument == "--receipt" && index + 1 < argc)
            receipt_path = argv[++index];
        else if (argument == "--first-frame-trial" && index + 1 < argc)
            first_frame_trial_path = argv[++index];
        else {
            std::cerr << "usage: pulp-gpu-dpr-native-measurement --request FILE "
                         "--receipt FILE\n";
            return 3;
        }
    }
    if (request_path.empty() ||
        (receipt_path.empty() == first_frame_trial_path.empty()) ||
        !regular_bounded_file(request_path)) {
        std::cerr << "invalid or unbounded DPR request file\n";
        return 3;
    }
    std::ifstream input{request_path, std::ios::binary};
    const std::string request{std::istreambuf_iterator<char>{input}, {}};
    std::string error;
    const auto parsed = probe::parse_dpr_measurement_request(request, &error);
    if (!parsed) {
        std::cerr << error << '\n';
        return 3;
    }
    const auto producer = std::filesystem::canonical(argv[0]);
    if (!first_frame_trial_path.empty())
        return probe::run_dpr_first_frame_trial(
            *parsed, first_frame_trial_path, producer, &error);
    return probe::run_dpr_measurement(
        *parsed, request_path, receipt_path, producer, &error);
}
