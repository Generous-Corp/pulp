#include "external_input_replay.hpp"
#include <fstream>
#include <regex>
#include <sstream>

namespace pulp::test {
namespace {
std::string field(const std::string& line, const char* key) {
    std::smatch m;
    if (std::regex_search(line, m, std::regex(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""))) return m[1];
    return {};
}
double number(const std::string& line, const char* key, double fallback = 0) {
    std::smatch m;
    if (std::regex_search(line, m, std::regex(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"))) return std::stod(m[1]);
    return fallback;
}
bool boolean(const std::string& line, const char* key, bool fallback = false) {
    auto p = line.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return fallback;
    p = line.find(':', p); if (p == std::string::npos) return fallback;
    return line.compare(p + 1, 4, "true") == 0;
}
runtime::ExternalInputStatus status(const std::string& value) {
    if (value == "valid") return runtime::ExternalInputStatus::valid;
    if (value == "invalid") return runtime::ExternalInputStatus::invalid;
    if (value == "denied") return runtime::ExternalInputStatus::denied;
    if (value == "paused") return runtime::ExternalInputStatus::paused;
    return runtime::ExternalInputStatus::unavailable;
}
}

ExternalInputReplayReceipt run_external_input_replay(const std::filesystem::path& path,
                                                       runtime::ExternalInputGate<64>& gate,
                                                       runtime::ExternalInputPolicy& policy) {
    ExternalInputReplayReceipt receipt; receipt.fixture = path.string();
    std::ifstream input(path); std::string line; std::size_t line_no = 0; std::uint64_t block = 128;
    while (std::getline(input, line)) {
        ++line_no; if (line_no == 1) { block = static_cast<std::uint64_t>(number(line, "block_samples", 128)); continue; }
        const auto type = field(line, "t");
        if (type == "frame") {
            runtime::ExternalInputFrame<64> frame;
            frame.source_id = static_cast<std::uint32_t>(number(line, "source_id"));
            frame.epoch = static_cast<std::uint32_t>(number(line, "epoch"));
            frame.sequence = static_cast<std::uint64_t>(number(line, "sequence"));
            frame.provider_time_ns = static_cast<std::uint64_t>(number(line, "provider_time_ns"));
            frame.status = status(field(line, "status")); frame.confidence = static_cast<float>(number(line, "confidence", 1));
            const auto begin = line.find("[", line.find("\"values\"")); const auto end = line.find("]", begin);
            if (begin != std::string::npos && end != std::string::npos) {
                std::stringstream values(line.substr(begin + 1, end - begin - 1)); std::string token;
                while (std::getline(values, token, ',') && frame.count < frame.capacity()) {
                    if (token.find("nan") != std::string::npos) frame.values[frame.count++] = std::numeric_limits<float>::quiet_NaN();
                    else frame.values[frame.count++] = std::stof(token);
                }
            }
            gate.submit(frame, policy); ++receipt.frames;
        } else if (type == "advance") {
            const auto blocks = static_cast<std::uint64_t>(number(line, "blocks"));
            gate.advance(blocks * block, policy); ++receipt.advances;
        } else if (type == "reset") { gate.expire(); ++receipt.resets;
        } else if (type == "expect") { ++receipt.expects; ++receipt.checked; if (line.find("\"valid\":true") != std::string::npos && !gate.valid()) ++receipt.failed; if (line.find("\"valid\":false") != std::string::npos && gate.valid()) ++receipt.failed; }
    }
    receipt.result = receipt.expects == 0 ? "inconclusive" : (receipt.failed == 0 ? "pass" : "fail");
    return receipt;
}

bool write_external_input_receipt(const std::filesystem::path& path,
                                  const ExternalInputReplayReceipt& r, std::string& error) {
    std::ofstream out(path); if (!out) { error = "cannot open receipt"; return false; }
    out << "{\"schema\":\"pulp.external-input-receipt.v1\",\"fixture\":\"" << r.fixture
        << "\",\"records\":{\"frames\":" << r.frames << ",\"advances\":" << r.advances
        << ",\"expects\":" << r.expects << ",\"resets\":" << r.resets
        << "},\"expectations\":{\"checked\":" << r.checked << ",\"failed\":" << r.failed
        << "},\"result\":\"" << r.result << "\"}\n";
    if (!out.good()) { error = "cannot write receipt"; return false; } return true;
}
} // namespace pulp::test
