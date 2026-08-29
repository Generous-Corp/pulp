#pragma once

#include <cstdint>
#include <optional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::tooling::gpu_probe {

inline constexpr std::string_view kDprCellRequestSchema =
    "pulp.gpu-dpr-cell-request.v1";
inline constexpr std::string_view kDprCellReceiptSchema =
    "pulp.gpu-dpr-cell-receipt.v1";

struct DprMeasurementRequest {
    std::string attempt_nonce;
    std::uint32_t attempt_number = 0;
    std::string cell_key;
    std::string scenario_id;
    std::string scenario_kind;
    std::string mode;
    double requested_dpr = 0.0;
    std::filesystem::path pulp_source_root;
    std::filesystem::path cell_directory;
    std::string source;
    std::string expected_content_digest;
    std::string pulp_sha;
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    double logical_input_x = 0.0;
    double logical_input_y = 0.0;
    std::string logical_input_target;
    std::uint32_t warmups = 0;
    std::uint32_t measured_trials = 0;
    std::uint32_t fresh_process_first_frame_trials = 0;
    std::uint32_t gpu_timer_calibration_trials = 0;
    std::uint32_t gpu_timer_extra_work_multiplier = 0;
    std::string adaptive_profile_json;
};

struct DprMeasurementDisposition {
    DprMeasurementRequest request;
    std::string outcome{"inconclusive"};
    std::string reason;
    std::vector<std::string> dependencies;
};

/// Parse the bounded runner request fields owned by the native producer.
std::optional<DprMeasurementRequest>
parse_dpr_measurement_request(std::string_view json, std::string* error = nullptr);

/// Report current product-measurement readiness without inventing counters.
DprMeasurementDisposition
evaluate_dpr_measurement_readiness(const DprMeasurementRequest& request);

/// Serialize the cell receipt consumed by the strict Python adapter.
std::string to_json(const DprMeasurementDisposition& result, bool pretty = false);

/// Execute a real bounded measurement. Returns the receipt exit code
/// (0 pass, 1 measured failure, 3 incomplete) and always writes a receipt.
int run_dpr_measurement(const DprMeasurementRequest& request,
                        const std::filesystem::path& request_path,
                        const std::filesystem::path& receipt_path,
                        const std::filesystem::path& producer_path,
                        std::string* error = nullptr);

int run_dpr_first_frame_trial(const DprMeasurementRequest& request,
                              const std::filesystem::path& output_path,
                              const std::filesystem::path& producer_path,
                              std::string* error = nullptr);

} // namespace pulp::tooling::gpu_probe
