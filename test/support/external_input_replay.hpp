#pragma once

#include <pulp/runtime/external_input.hpp>
#include <filesystem>
#include <string>

namespace pulp::test {

struct ExternalInputReplayReceipt {
    std::string fixture;
    std::size_t frames = 0, advances = 0, expects = 0, resets = 0;
    std::size_t checked = 0, failed = 0;
    std::string result;
};

ExternalInputReplayReceipt run_external_input_replay(
    const std::filesystem::path& path,
    runtime::ExternalInputGate<64>& gate,
    runtime::ExternalInputPolicy& policy);

bool write_external_input_receipt(const std::filesystem::path& path,
                                  const ExternalInputReplayReceipt& receipt,
                                  std::string& error);

} // namespace pulp::test
