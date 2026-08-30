#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const auto* evidence_path = std::getenv("PULP_TEST_GPU_PROBE_EVIDENCE");
    const auto* argv_path = std::getenv("PULP_TEST_GPU_PROBE_ARGV");
    const auto* status_text = std::getenv("PULP_TEST_GPU_PROBE_STATUS");
    if (evidence_path == nullptr || argv_path == nullptr || status_text == nullptr)
        return 125;

    std::ofstream recorded(argv_path, std::ios::trunc);
    for (int i = 1; i < argc; ++i) recorded << argv[i] << '\n';
    recorded.close();

    if (const auto* delay_text = std::getenv("PULP_TEST_GPU_PROBE_DELAY_MS"))
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(delay_text)));
    if (const auto* stderr_bytes_text = std::getenv("PULP_TEST_GPU_PROBE_STDERR_BYTES")) {
        auto remaining = static_cast<std::size_t>(std::stoull(stderr_bytes_text));
        const std::string chunk(4096, 'x');
        while (remaining != 0) {
            const auto count = std::min(remaining, chunk.size());
            std::cerr.write(chunk.data(), static_cast<std::streamsize>(count));
            remaining -= count;
        }
    }

    std::ifstream evidence(evidence_path);
    std::cout << evidence.rdbuf();
    if (const auto* stdout_bytes_text =
            std::getenv("PULP_TEST_GPU_PROBE_STDOUT_TRAILING_BYTES")) {
        auto remaining = static_cast<std::size_t>(std::stoull(stdout_bytes_text));
        const std::string chunk(4096, 'x');
        while (remaining != 0) {
            const auto count = std::min(remaining, chunk.size());
            std::cout.write(chunk.data(), static_cast<std::streamsize>(count));
            remaining -= count;
        }
    }
    return std::stoi(status_text);
}
