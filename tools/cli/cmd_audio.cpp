// cmd_audio.cpp — pulp audio command

#include "cli_common.hpp"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_scope.hpp>
#include <pulp/audio/audio_scope_json.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/render/headless_surface.hpp>

#include "cmd_audio_render.hpp"
#include "cmd_audio_validate.hpp"

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
    std::cout << "  pulp audio scope [target] --frames 90 --window 2048 --trigger rising-zero --channel 0 [--json <path>]\n";
    std::cout << "  pulp audio scope --input-wav <path> --window 2048 --trigger rising-zero --channel 0 [--json <path>] [--png <path>]\n";
    std::cout << "  pulp audio validate <verb> ...   (summarize|doctor|compare|assert)\n";
    std::cout << "  pulp audio render --plugin <bundle> --out <file.wav> (--duration-ms <n> | --duration-frames <n>) [options]\n";
}

namespace {

bool parse_positive_int_arg(const std::string& text, const char* flag, int& out) {
    std::size_t parsed = 0;
    if (!parse_size_arg(text, flag, parsed)) return false;
    if (parsed == 0 || parsed > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        std::cerr << "Error: " << flag << " must be a positive integer\n";
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_nonnegative_int_arg(const std::string& text, const char* flag, int& out) {
    std::size_t parsed = 0;
    if (!parse_size_arg(text, flag, parsed)) return false;
    if (parsed > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        std::cerr << "Error: " << flag << " is too large\n";
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool valid_scope_trigger(const std::string& trigger) {
    return trigger == "none" || trigger == "off" || trigger == "raw"
        || trigger == "rising-zero" || trigger == "rising_zero";
}

bool ensure_parent_dir(const fs::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) return true;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        std::cerr << "Error: failed to create directory " << parent << "\n";
        return false;
    }
    return true;
}

std::string read_text_file_or_empty(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool write_binary_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    if (!ensure_parent_dir(path)) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::vector<std::uint8_t> render_scope_png(
    const pulp::audio::AudioScopeAcquisition& acquisition,
    std::string* error_out) {
    constexpr std::uint32_t width = 900;
    constexpr std::uint32_t height = 360;
    pulp::render::HeadlessSurface::Rgba rgba;
    rgba.width = width;
    rgba.height = height;
    rgba.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);

    auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b, std::uint8_t a = 255) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height)) {
            return;
        }
        const auto idx = (static_cast<std::size_t>(y) * width +
                          static_cast<std::size_t>(x)) * 4;
        rgba.pixels[idx + 0] = r;
        rgba.pixels[idx + 1] = g;
        rgba.pixels[idx + 2] = b;
        rgba.pixels[idx + 3] = a;
    };

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x)
            put(static_cast<int>(x), static_cast<int>(y), 18, 18, 22);
    }
    for (std::uint32_t x = 0; x < width; x += width / 8)
        for (std::uint32_t y = 0; y < height; ++y)
            put(static_cast<int>(x), static_cast<int>(y), 36, 42, 50);
    for (std::uint32_t y = 0; y < height; y += height / 4)
        for (std::uint32_t x = 0; x < width; ++x)
            put(static_cast<int>(x), static_cast<int>(y), 36, 42, 50);
    for (std::uint32_t x = 0; x < width; ++x)
        put(static_cast<int>(x), static_cast<int>(height / 2), 70, 80, 95);

    const auto& samples = acquisition.samples;
    if (samples.size() >= 2) {
        auto point = [&](std::size_t i) {
            const float x = static_cast<float>(i) /
                static_cast<float>(samples.size() - 1);
            const float s = std::clamp(samples[i], -1.0f, 1.0f);
            return std::pair<int, int>{
                static_cast<int>(std::round(x * static_cast<float>(width - 1))),
                static_cast<int>(std::round(
                    static_cast<float>(height) * 0.5f -
                    s * static_cast<float>(height) * 0.42f))
            };
        };
        auto [prev_x, prev_y] = point(0);
        for (std::size_t i = 1; i < samples.size(); ++i) {
            auto [next_x, next_y] = point(i);
            const int dx = std::abs(next_x - prev_x);
            const int dy = -std::abs(next_y - prev_y);
            const int sx = prev_x < next_x ? 1 : -1;
            const int sy = prev_y < next_y ? 1 : -1;
            int err = dx + dy;
            int x = prev_x;
            int y = prev_y;
            while (true) {
                put(x, y, 90, 200, 140);
                put(x, y + 1, 70, 170, 120);
                if (x == next_x && y == next_y) break;
                const int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x += sx; }
                if (e2 <= dx) { err += dx; y += sy; }
            }
            prev_x = next_x;
            prev_y = next_y;
        }
    }

    return pulp::render::HeadlessSurface::encode_png(rgba, error_out);
}

int cmd_audio_scope_offline(const fs::path& input_wav,
                            const fs::path& json_path,
                            const fs::path& png_path,
                            int window,
                            int channel,
                            const std::string& trigger) {
    auto data = pulp::audio::read_audio_file(input_wav.string());
    if (!data || data->empty()) {
        std::cerr << "Error: failed to read WAV input " << input_wav << "\n";
        return 1;
    }

    std::vector<const float*> ptrs;
    ptrs.reserve(data->channels.size());
    for (const auto& ch : data->channels)
        ptrs.push_back(ch.data());

    pulp::audio::BufferView<const float> view(
        ptrs.data(), ptrs.size(), static_cast<std::size_t>(data->num_frames()));

    pulp::audio::AudioScopeTriggerMode trigger_mode =
        pulp::audio::AudioScopeTriggerMode::kRisingZero;
    if (!pulp::audio::parse_audio_scope_trigger_mode(trigger, trigger_mode)) {
        std::cerr << "Error: --trigger must be one of none, raw, off, rising-zero\n";
        return 1;
    }

    pulp::audio::AudioProbeSnapshot meta;
    meta.sample_rate = data->sample_rate;
    meta.channel_count = data->num_channels();
    meta.stage_id = pulp::audio::AudioProbeStage::kUnknown;

    pulp::audio::AudioScopeResult result;
    result.stage = meta.stage_id;
    result.source_kind = "input_wav";
    result.source_path = input_wav.string();
    result.trigger_mode = trigger_mode;

    pulp::audio::AudioScopeAcquisitionConfig config;
    config.window_samples = static_cast<std::uint32_t>(window);
    config.trigger_mode = trigger_mode;
    config.selected_channel = static_cast<std::uint32_t>(channel);
    result.acquisition = pulp::audio::acquire_audio_scope_window(view, config, &meta);
    result.measurements = pulp::audio::measure_audio_scope_window(result.acquisition);

    const std::string json = pulp::audio::audio_scope_result_to_json(result, true);
    if (!json_path.empty()) {
        if (!ensure_parent_dir(json_path)) return 1;
        std::ofstream out(json_path);
        if (!out.is_open()) {
            std::cerr << "Error: failed to write JSON " << json_path << "\n";
            return 1;
        }
        out << json;
        if (!out.good()) {
            std::cerr << "Error: failed to finish JSON write " << json_path << "\n";
            return 1;
        }
    } else {
        std::cout << json << "\n";
    }

    if (!png_path.empty()) {
        std::string png_error;
        auto png = render_scope_png(result.acquisition, &png_error);
        if (png.empty()) {
            std::cerr << "Error: failed to encode scope PNG";
            if (!png_error.empty()) std::cerr << ": " << png_error;
            std::cerr << "\n";
            return 1;
        }
        if (!write_binary_file(png_path, png)) {
            std::cerr << "Error: failed to write PNG " << png_path << "\n";
            return 1;
        }
    }
    return 0;
}

int cmd_audio_scope(const std::vector<std::string>& args) {
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        std::cout
            << "pulp audio scope — capture live output-boundary scope JSON\n\n"
               "Usage: pulp audio scope [target] [--frames <n>] [--window <samples>]\n"
               "                        [--trigger none|raw|off|rising-zero]\n"
               "                        [--channel <index>] [--json <path>]\n"
               "       pulp audio scope --input-wav <path> [--window <samples>]\n"
               "                        [--trigger none|raw|off|rising-zero]\n"
               "                        [--channel <index>] [--json <path>] [--png <path>]\n\n"
               "This wraps `pulp run --audio-scope-json`, runs headlessly, and still\n"
               "opens the live standalone audio device. Use Audio Doctor/offline paths\n"
               "for guaranteed speakerless tests.\n";
        return 0;
    }

    std::string target;
    std::string json_path;
    std::string input_wav;
    std::string png_path;
    int frames = 90;
    int window = 2048;
    int channel = 0;
    std::string trigger = "rising-zero";

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto require_value = [&](const char* flag) -> const std::string* {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value.\n";
                return nullptr;
            }
            return &args[++i];
        };

        if (arg == "--frames") {
            auto* value = require_value("--frames");
            if (!value || !parse_positive_int_arg(*value, "--frames", frames)) return 1;
        } else if (arg == "--window") {
            auto* value = require_value("--window");
            if (!value || !parse_positive_int_arg(*value, "--window", window)) return 1;
        } else if (arg == "--trigger") {
            auto* value = require_value("--trigger");
            if (!value) return 1;
            trigger = *value;
            if (!valid_scope_trigger(trigger)) {
                std::cerr << "Error: --trigger must be one of none, raw, off, rising-zero\n";
                return 1;
            }
        } else if (arg == "--channel") {
            auto* value = require_value("--channel");
            if (!value || !parse_nonnegative_int_arg(*value, "--channel", channel)) return 1;
        } else if (arg == "--json") {
            auto* value = require_value("--json");
            if (!value || value->empty()) {
                std::cerr << "Error: --json requires a non-empty path.\n";
                return 1;
            }
            json_path = *value;
        } else if (arg == "--input-wav") {
            auto* value = require_value("--input-wav");
            if (!value || value->empty()) {
                std::cerr << "Error: --input-wav requires a non-empty path.\n";
                return 1;
            }
            input_wav = *value;
        } else if (arg == "--png") {
            auto* value = require_value("--png");
            if (!value || value->empty()) {
                std::cerr << "Error: --png requires a non-empty path.\n";
                return 1;
            }
            png_path = *value;
        } else if (arg == "--include-samples") {
            std::cerr << "Error: " << arg << " is not yet supported for `pulp audio scope`.\n";
            return 1;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        } else if (target.empty()) {
            target = arg;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    if (!input_wav.empty()) {
        if (!target.empty()) {
            std::cerr << "Error: --input-wav cannot be combined with a live target.\n";
            return 1;
        }
        return cmd_audio_scope_offline(input_wav, json_path, png_path,
                                       window, channel, trigger);
    }

    if (!png_path.empty()) {
        std::cerr << "Error: --png is only supported with --input-wav.\n";
        return 1;
    }

    bool temp_output = false;
    fs::path output_path = json_path;
    fs::path temp_dir;
    if (output_path.empty()) {
        temp_output = true;
        temp_dir = fs::temp_directory_path()
            / ("pulp-audio-scope-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(temp_dir, ec);
        if (ec) {
            std::cerr << "Error: failed to create temp directory for scope JSON\n";
            return 1;
        }
        output_path = temp_dir / "scope.json";
    }

    std::string cmd = shell_quote(current_executable_path()) + " run";
    if (!target.empty()) cmd += " " + shell_quote(target);
    cmd += " --audio-scope-json " + shell_quote(output_path);
    cmd += " --audio-scope-window " + std::to_string(window);
    cmd += " --audio-scope-trigger " + shell_quote(trigger);
    cmd += " --audio-scope-channel " + std::to_string(channel);
    cmd += " --frames " + std::to_string(frames);

    fs::path log_path;
    if (temp_output) {
        log_path = temp_dir / "run.log";
        cmd += " > " + shell_quote(log_path) + " 2>&1";
    }
    const int rc = run(cmd);
    if (rc != 0) {
        if (temp_output) {
            auto log = read_text_file_or_empty(log_path);
            if (!log.empty()) std::cerr << log << "\n";
        }
        if (temp_output) {
            std::error_code ec;
            fs::remove_all(temp_dir, ec);
        }
        return rc;
    }

    if (temp_output) {
        auto json = read_text_file_or_empty(output_path);
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        if (json.empty()) {
            std::cerr << "Error: pulp run did not write audio scope JSON\n";
            return 1;
        }
        std::cout << json << "\n";
    }
    return 0;
}

}  // namespace

int cmd_audio(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_audio_usage();
        return 0;
    }

    if (args[0] == "validate") {
        return cmd_audio_validate({args.begin() + 1, args.end()});
    }

    if (args[0] == "render") {
        return cmd_audio_render({args.begin() + 1, args.end()});
    }

    if (args[0] == "scope") {
        return cmd_audio_scope({args.begin() + 1, args.end()});
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
