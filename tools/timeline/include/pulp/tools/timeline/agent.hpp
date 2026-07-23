#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::tools::timeline {

struct OperationResult {
    int exit_code = 0;
    std::string json;

    explicit operator bool() const noexcept {
        return exit_code == 0;
    }
};

OperationResult project_open(std::string_view project);
OperationResult command_apply(std::string_view project, std::string_view commands);
OperationResult validate(std::string_view project);
OperationResult explain(std::string_view project, std::uint32_t sample_rate = 48'000);
OperationResult render(std::string_view project, std::string_view output,
                       std::uint32_t sample_rate = 48'000);
OperationResult schema();

} // namespace pulp::tools::timeline
