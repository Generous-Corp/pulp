#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const auto* evidence_path = std::getenv("PULP_TEST_GPU_PROBE_EVIDENCE");
    const auto* argv_path = std::getenv("PULP_TEST_GPU_PROBE_ARGV");
    const auto* status_text = std::getenv("PULP_TEST_GPU_PROBE_STATUS");
    if (evidence_path == nullptr || argv_path == nullptr || status_text == nullptr)
        return 125;

    std::ofstream recorded(argv_path, std::ios::trunc);
    for (int i = 1; i < argc; ++i) recorded << argv[i] << '\n';
    recorded.close();

    std::ifstream evidence(evidence_path);
    std::cout << evidence.rdbuf();
    return std::stoi(status_text);
}
