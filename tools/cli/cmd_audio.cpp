// cmd_audio.cpp — pulp audio command

#include "cli_common.hpp"

#include <cerrno>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>

#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>

// Parsing helpers (defined in cli_common.cpp, not in header since they are only
// used by audio command — declare them locally to keep the header lean).
extern bool parse_size_arg(const std::string& text, const char* flag, std::size_t& out);
extern bool parse_double_arg(const std::string& text, const char* flag, double& out);

static void print_audio_usage() {
    std::cout << "pulp audio — repo-level audio analysis tooling\n\n";
    std::cout << "Usage:\n";
    std::cout << "  pulp audio model list [--json]\n";
    std::cout << "  pulp audio model status [--json]\n";
    std::cout << "  pulp audio model activate <model-id> [--json]\n";
    std::cout << "  pulp audio excerpt-find --text <query> --input <path> [options]\n";
    std::cout << "  pulp audio read-bundle <path> [--json]\n";
}

int cmd_audio(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_audio_usage();
        return 0;
    }

    if (args[0] == "model") {
        if (args.size() < 2) {
            std::cerr << "Unknown audio model subcommand.\n";
            print_audio_usage();
            return 1;
        }

        if (args[1] == "list") {
            bool json_output = false;
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--json") json_output = true;
                else {
                    std::cerr << "Unknown option: " << args[i] << "\n";
                    return 1;
                }
            }

            auto result = pulp::tools::audio::list_models();
            if (json_output) {
                std::cout << pulp::tools::audio::to_json(result) << "\n";
                return result.error.empty() ? 0 : 1;
            }

            std::cout << "Audio Models\n";
            std::cout << "============\n";
            std::cout << "Active: " << (result.active_model_id.empty() ? "(none)" : result.active_model_id) << "\n";
            for (const auto& item : result.models) {
                std::cout << (item.active ? "* " : "  ")
                          << item.model.model_id
                          << " [" << item.status << "]"
                          << " backend=" << item.model.backend;
                if (!item.model.task_tags.empty()) {
                    std::cout << " tags=";
                    for (std::size_t i = 0; i < item.model.task_tags.size(); ++i) {
                        if (i > 0) std::cout << ",";
                        std::cout << item.model.task_tags[i];
                    }
                }
                std::cout << "\n";
            }
            if (!result.error.empty()) {
                std::cerr << "Error: " << result.error << "\n";
                return 1;
            }
            return 0;
        }

        if (args[1] == "activate") {
            if (args.size() < 3) {
                std::cerr << "Error: model id is required.\n";
                print_audio_usage();
                return 1;
            }

            std::string model_id;
            bool json_output = false;
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--json") json_output = true;
                else if (model_id.empty()) model_id = args[i];
                else {
                    std::cerr << "Unknown argument: " << args[i] << "\n";
                    return 1;
                }
            }

            auto result = pulp::tools::audio::activate_model(model_id);
            if (json_output) {
                std::cout << pulp::tools::audio::to_json(result) << "\n";
                return result.ok ? 0 : 1;
            }

            if (!result.ok) {
                std::cerr << "Error: " << result.error << "\n";
                return 1;
            }

            std::cout << "Activated audio model: " << result.active_model_id << "\n";
            std::cout << "State file: " << result.state_path.string() << "\n";
            return 0;
        }

        if (args[1] != "status") {
            std::cerr << "Unknown audio model subcommand.\n";
            print_audio_usage();
            return 1;
        }

        bool json_output = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--json") json_output = true;
            else {
                std::cerr << "Unknown option: " << args[i] << "\n";
                return 1;
            }
        }

        auto status = pulp::tools::audio::query_model_status();
        if (json_output) {
            std::cout << pulp::tools::audio::to_json(status) << "\n";
            return 0;
        }

        std::cout << "Audio Model Status\n";
        std::cout << "==================\n";
        std::cout << "State file: " << (status.state_path.empty() ? "(unresolved)" : status.state_path.string()) << "\n";
        std::cout << "State file found: " << (status.state_file_found ? "yes" : "no") << "\n";
        std::cout << "Configured model: "
                  << (status.configured_model_id.empty() ? "(none)" : status.configured_model_id) << "\n";
        std::cout << "Backend: " << (status.backend.empty() ? "(unknown)" : status.backend) << "\n";
        std::cout << "Resolved checkpoint: "
                  << (status.resolved_checkpoint_path.empty() ? "(none)" : status.resolved_checkpoint_path.string())
                  << "\n";
        std::cout << "Loadable: " << (status.loadable() ? "yes" : "no") << "\n";
        std::cout << "Message: " << status.message << "\n";
        return 0;
    }

    if (args[0] == "excerpt-find") {
        pulp::tools::audio::ExcerptFindRequest request;
        bool json_output = false;

        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& arg = args[i];
            auto require_value = [&](const char* flag) -> const std::string* {
                if (i + 1 >= args.size()) {
                    std::cerr << "Error: " << flag << " requires a value.\n";
                    return nullptr;
                }
                return &args[++i];
            };

            if (arg == "--text") {
                auto* value = require_value("--text");
                if (!value) return 1;
                request.text = *value;
            } else if (arg == "--input") {
                auto* value = require_value("--input");
                if (!value) return 1;
                request.input_path = *value;
            } else if (arg == "--model") {
                auto* value = require_value("--model");
                if (!value) return 1;
                request.model_id = *value;
            } else if (arg == "--recursive") {
                request.recursive = true;
            } else if (arg == "--top") {
                auto* value = require_value("--top");
                if (!value) return 1;
                if (!parse_size_arg(*value, "--top", request.top_k)) return 1;
            } else if (arg == "--window-ms") {
                auto* value = require_value("--window-ms");
                if (!value) return 1;
                uint64_t v = 0;
                // reuse parse logic inline
                errno = 0;
                char* end = nullptr;
                v = std::strtoull(value->c_str(), &end, 10);
                if (errno == ERANGE || end != value->c_str() + value->size()) {
                    std::cerr << "Error: invalid value for --window-ms: " << *value << "\n";
                    return 1;
                }
                request.window_ms = v;
            } else if (arg == "--hop-ms") {
                auto* value = require_value("--hop-ms");
                if (!value) return 1;
                uint64_t v = 0;
                errno = 0;
                char* end = nullptr;
                v = std::strtoull(value->c_str(), &end, 10);
                if (errno == ERANGE || end != value->c_str() + value->size()) {
                    std::cerr << "Error: invalid value for --hop-ms: " << *value << "\n";
                    return 1;
                }
                request.hop_ms = v;
            } else if (arg == "--min-score") {
                auto* value = require_value("--min-score");
                if (!value) return 1;
                if (!parse_double_arg(*value, "--min-score", request.min_score)) return 1;
            } else if (arg == "--max-candidates-per-file") {
                auto* value = require_value("--max-candidates-per-file");
                if (!value) return 1;
                if (!parse_size_arg(*value, "--max-candidates-per-file", request.max_candidates_per_file)) return 1;
            } else if (arg == "--bundle-out") {
                auto* value = require_value("--bundle-out");
                if (!value) return 1;
                request.bundle_out = *value;
            } else if (arg == "--json") {
                json_output = true;
            } else if (arg == "--dry-run") {
                request.dry_run = true;
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        auto result = pulp::tools::audio::run_excerpt_find(request);
        if (json_output) {
            std::cout << pulp::tools::audio::to_json(result) << "\n";
            return result.ok ? 0 : 1;
        }

        if (!result.ok) {
            std::cerr << "Error: " << result.error << "\n";
            return 1;
        }

        std::cout << "Audio Excerpt Search\n";
        std::cout << "====================\n";
        if (!result.bundle_path.empty())
            std::cout << "Bundle: " << result.bundle_path.string() << "\n";
        std::cout << "Query: " << result.query << "\n";
        std::cout << "Requested model: " << result.requested_model_id << "\n";
        std::cout << "Loaded model: " << result.loaded_model_id << "\n";
        std::cout << "Backend: " << result.backend << " (WAV-first deterministic stub)\n";
        std::cout << "Scanned files: " << result.scanned_file_count << "\n";
        for (const auto& item : result.results) {
            std::cout << "  #" << item.rank
                      << " score=" << std::fixed << std::setprecision(4) << item.score
                      << " source=" << item.source_file
                      << " [" << item.start_ms << "ms, " << item.end_ms << "ms]"
                      << "\n";
        }
        return 0;
    }

    if (args[0] == "read-bundle") {
        if (args.size() < 2) {
            std::cerr << "Error: bundle path is required.\n";
            print_audio_usage();
            return 1;
        }

        fs::path bundle_path;
        bool json_output = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--json") {
                json_output = true;
                continue;
            }
            if (bundle_path.empty()) {
                bundle_path = args[i];
                continue;
            }
            std::cerr << "Unknown argument: " << args[i] << "\n";
            return 1;
        }

        auto bundle = pulp::tools::audio::read_excerpt_bundle(bundle_path);
        if (json_output) {
            std::cout << pulp::tools::audio::to_json(bundle) << "\n";
            return bundle.ok ? 0 : 1;
        }

        if (!bundle.ok) {
            std::cerr << "Error: " << bundle.error << "\n";
            return 1;
        }

        std::cout << "Audio Excerpt Bundle\n";
        std::cout << "====================\n";
        std::cout << "Bundle: " << bundle.bundle_path.string() << "\n";
        std::cout << "Tool: " << (bundle.tool.empty() ? "(unknown)" : bundle.tool) << "\n";
        std::cout << "Requested model: "
                  << (bundle.requested_model_id.empty() ? "(unknown)" : bundle.requested_model_id) << "\n";
        std::cout << "Loaded model: "
                  << (bundle.loaded_model_id.empty() ? "(unknown)" : bundle.loaded_model_id) << "\n";
        std::cout << "Backend: " << (bundle.backend.empty() ? "(unknown)" : bundle.backend) << "\n";
        std::cout << "Results: " << bundle.result_count << "\n";
        for (const auto& item : bundle.results) {
            std::cout << "  #" << item.rank
                      << " score=" << std::fixed << std::setprecision(4) << item.score
                      << " source=" << item.source_file
                      << " [" << item.start_ms << "ms, " << item.end_ms << "ms]"
                      << "\n";
        }
        return 0;
    }

    std::cerr << "Unknown audio subcommand.\n";
    print_audio_usage();
    return 1;
}
