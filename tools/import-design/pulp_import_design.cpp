#include <pulp/view/design_import.hpp>
#include <pulp/view/design_export.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/state/store.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;
using namespace pulp::view;
using namespace pulp::state;

static void print_usage() {
    std::cout << "pulp import-design — Import designs from external tools into Pulp\n\n";
    std::cout << "Usage:\n";
    std::cout << "  pulp import-design --from <source> [options]\n\n";
    std::cout << "Sources:\n";
    std::cout << "  figma    Figma export JSON or MCP data\n";
    std::cout << "  stitch   Google Stitch screen HTML or MCP data\n";
    std::cout << "  v0       v0.dev TSX/Tailwind output\n";
    std::cout << "  pencil   Pencil/OpenPencil node JSON or .pen export\n\n";
    std::cout << "Options:\n";
    std::cout << "  --from <source>   Design source (required)\n";
    std::cout << "  --file <path>     Input file path\n";
    std::cout << "  --url <url>       Design URL (Figma file URL or v0 share link)\n";
    std::cout << "  --frame <name>    Frame/artboard to import (Figma)\n";
    std::cout << "  --screen <name>   Screen to import (Stitch)\n";
    std::cout << "  --output <path>   Output JS file (default: ui.js)\n";
    std::cout << "  --tokens <path>   Output W3C token file (default: tokens.json)\n";
    std::cout << "  --dry-run         Show generated code without writing files\n";
    std::cout << "  --no-tokens       Skip token extraction\n";
    std::cout << "  --no-comments     Omit comments from generated code\n";
    std::cout << "  --web-compat      Use DOM API instead of native Pulp API\n";
    std::cout << "  --validate        Render generated JS and validate layout\n";
    std::cout << "  --reference <png> Compare render against a reference screenshot\n";
    std::cout << "  --diff <png>      Save visual diff image\n";
    std::cout << "  --render-size WxH Render dimensions (default: 340x280)\n";
    std::cout << "  --help            Show this help\n\n";
    std::cout << "Examples:\n";
    std::cout << "  pulp import-design --from figma --file design.json\n";
    std::cout << "  pulp import-design --from figma --url 'https://figma.com/design/...' --frame 'Plugin UI'\n";
    std::cout << "  pulp import-design --from stitch --file screen.html --screen 'Main'\n";
    std::cout << "  pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output my-ui.js\n";
    std::cout << "  pulp import-design --from pencil --file design.json --dry-run\n";
    std::cout << "  pulp import-design --from pencil --file design.json --validate --reference source.png\n";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Error: cannot open file: " << path << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& content) {
    // Create parent directories if needed
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "Error: cannot write file: " << path << "\n";
        return false;
    }
    f << content;
    return true;
}

int main(int argc, char* argv[]) {
    std::string source_str;
    std::string input_file;
    std::string input_url;           // --url: Figma file URL or v0 share link
    std::string frame_name;          // --frame: Figma frame/artboard name
    std::string screen_name;         // --screen: Stitch screen name
    std::string output_file = "ui.js";
    std::string tokens_file = "tokens.json";
    std::string export_format = "w3c";
    std::string reference_image;     // --reference: PNG of source design for validation
    std::string diff_output;         // --diff: output path for visual diff image
    bool dry_run = false;
    bool include_tokens = true;
    bool include_comments = true;
    bool export_tokens_mode = false;
    bool validate = false;           // --validate: render + compare after import
    bool use_web_compat = false;     // --web-compat: use DOM API instead of native
    bool preview_mode = false;       // --preview: minimal widget style for design comparison
    bool debug_json = false;         // --debug: output JSON report with all metrics
    std::string debug_output;        // --debug-output: path for JSON report
    int render_width = 340;
    int render_height = 280;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            source_str = argv[++i];
        } else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            input_url = argv[++i];
        } else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
            frame_name = argv[++i];
        } else if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            screen_name = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (std::strcmp(argv[i], "--tokens") == 0 && i + 1 < argc) {
            tokens_file = argv[++i];
        } else if (std::strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (std::strcmp(argv[i], "--no-tokens") == 0) {
            include_tokens = false;
        } else if (std::strcmp(argv[i], "--no-comments") == 0) {
            include_comments = false;
        } else if (std::strcmp(argv[i], "--export-tokens") == 0) {
            export_tokens_mode = true;
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            export_format = argv[++i];
        } else if (std::strcmp(argv[i], "--web-compat") == 0) {
            use_web_compat = true;
        } else if (std::strcmp(argv[i], "--validate") == 0) {
            validate = true;
        } else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc) {
            reference_image = argv[++i];
            validate = true;
        } else if (std::strcmp(argv[i], "--diff") == 0 && i + 1 < argc) {
            diff_output = argv[++i];
        } else if (std::strcmp(argv[i], "--render-size") == 0 && i + 1 < argc) {
            // Parse WxH
            std::string sz = argv[++i];
            auto x = sz.find('x');
            if (x != std::string::npos) {
                render_width = std::stoi(sz.substr(0, x));
                render_height = std::stoi(sz.substr(x + 1));
            }
        } else if (std::strcmp(argv[i], "--preview") == 0) {
            preview_mode = true;
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            debug_json = true;
        } else if (std::strcmp(argv[i], "--debug-output") == 0 && i + 1 < argc) {
            debug_output = argv[++i];
            debug_json = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    // Export-tokens mode: read a Pulp theme JSON and export as W3C tokens
    if (export_tokens_mode) {
        if (input_file.empty()) {
            // No input = export the built-in dark theme
            auto theme = Theme::dark();
            auto w3c = export_w3c_tokens(theme);
            if (dry_run) {
                std::cout << w3c;
                return 0;
            }
            if (!write_file(tokens_file, w3c)) return 1;
            std::cout << "Exported " << (theme.colors.size() + theme.dimensions.size() + theme.strings.size())
                      << " tokens → " << tokens_file << "\n";
            return 0;
        }
        // Read theme JSON → export as W3C
        auto content = read_file(input_file);
        if (content.empty()) return 1;
        auto theme = Theme::from_json(content);
        auto w3c = export_w3c_tokens(theme);
        if (dry_run) {
            std::cout << w3c;
            return 0;
        }
        if (!write_file(tokens_file, w3c)) return 1;
        std::cout << "Exported " << (theme.colors.size() + theme.dimensions.size() + theme.strings.size())
                  << " tokens → " << tokens_file << "\n";
        return 0;
    }

    if (source_str.empty()) {
        std::cerr << "Error: --from <source> is required\n";
        print_usage();
        return 1;
    }

    auto source = parse_design_source(source_str);
    if (!source) {
        std::cerr << "Error: unknown source '" << source_str << "'\n";
        std::cerr << "Valid sources: figma, stitch, v0, pencil\n";
        return 1;
    }

    if (input_file.empty() && input_url.empty()) {
        std::cerr << "Error: --file <path> or --url <url> is required\n";
        return 1;
    }

    // --url without --file: fetch the URL content via curl
    std::string fetched_tmp;
    if (input_file.empty() && !input_url.empty()) {
        fetched_tmp = fs::temp_directory_path() / "pulp-import-fetched.tmp";
        std::string cmd = "curl -sL -o '" + fetched_tmp + "' '" + input_url + "' 2>&1";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "Error: failed to fetch URL: " << input_url << "\n";
            return 1;
        }
        input_file = fetched_tmp;
        std::cout << "Fetched " << input_url << " → " << fetched_tmp << "\n";
    }

    auto t_start = std::chrono::steady_clock::now();

    // Read input
    auto content = read_file(input_file);
    if (content.empty()) return 1;

    // Parse based on source
    DesignIR ir;
    try {
        switch (*source) {
            case DesignSource::figma:  ir = parse_figma_json(content); break;
            case DesignSource::stitch: ir = parse_stitch_html(content); break;
            case DesignSource::v0:     ir = parse_v0_tsx(content); break;
            case DesignSource::pencil: ir = parse_pencil_json(content); break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing " << design_source_name(*source) << " input: " << e.what() << "\n";
        return 1;
    }

    ir.source = *source;
    ir.source_file = input_url.empty() ? input_file : input_url;

    // Clean up temp file from URL fetch
    if (!fetched_tmp.empty()) fs::remove(fetched_tmp);

    // Store frame/screen selection metadata
    if (!frame_name.empty()) ir.root.attributes["frame"] = frame_name;
    if (!screen_name.empty()) ir.root.attributes["screen"] = screen_name;

    // Generate Pulp JS
    CodeGenOptions opts;
    opts.mode = use_web_compat ? CodeGenMode::web_compat : CodeGenMode::native;
    opts.include_tokens = include_tokens;
    opts.include_comments = include_comments;
    opts.preview_mode = preview_mode;
    auto js = generate_pulp_js(ir, opts);

    if (dry_run) {
        std::cout << "=== Generated Pulp JS (" << design_source_name(*source) << " → " << output_file << ") ===\n\n";
        std::cout << js;

        if (include_tokens && (!ir.tokens.colors.empty() || !ir.tokens.dimensions.empty())) {
            auto theme = ir_tokens_to_theme(ir.tokens);
            auto w3c = export_w3c_tokens(theme);
            std::cout << "\n=== W3C Design Tokens (" << tokens_file << ") ===\n\n";
            std::cout << w3c;
        }
        return 0;
    }

    auto t_codegen = std::chrono::steady_clock::now();

    // Write output files
    if (!write_file(output_file, js)) return 1;

    // Count elements by type
    size_t node_count = 0, text_count = 0, container_count = 0, widget_count = 0;
    std::function<void(const IRNode&)> count_nodes = [&](const IRNode& n) {
        node_count++;
        if (n.audio_widget != AudioWidgetType::none) widget_count++;
        else if (n.type == "text" || n.type == "label") text_count++;
        else if (!n.children.empty() || n.type == "frame") container_count++;
        for (auto& c : n.children) count_nodes(c);
    };
    count_nodes(ir.root);

    auto t_write = std::chrono::steady_clock::now();
    auto ms_total = std::chrono::duration_cast<std::chrono::milliseconds>(t_write - t_start).count();
    auto ms_codegen = std::chrono::duration_cast<std::chrono::milliseconds>(t_codegen - t_start).count();

    std::cout << "Wrote " << output_file << " (" << node_count << " elements: "
              << container_count << " containers, " << widget_count << " widgets, "
              << text_count << " labels";

    // Write tokens
    if (include_tokens && (!ir.tokens.colors.empty() || !ir.tokens.dimensions.empty() || !ir.tokens.strings.empty())) {
        auto theme = ir_tokens_to_theme(ir.tokens);
        auto w3c = export_w3c_tokens(theme);
        if (write_file(tokens_file, w3c)) {
            size_t token_count = ir.tokens.colors.size() + ir.tokens.dimensions.size() + ir.tokens.strings.size();
            std::cout << ", " << token_count << " tokens → " << tokens_file;
        }
    }

    std::cout << ")\n";

    // ── Validation: render generated JS and compare with reference ──────
    if (validate) {
        std::cout << "Validating render...\n";

        // Render the generated JS headlessly
        View render_root;
        render_root.set_theme(Theme::dark());
        render_root.flex().direction = FlexDirection::column;
        StateStore render_store;
        ScriptEngine render_engine;
        WidgetBridge render_bridge(render_engine, render_root, render_store);
        try {
            render_bridge.load_script(js);
        } catch (const std::exception& e) {
            std::cerr << "Validation error: generated JS failed to load: " << e.what() << "\n";
            return 1;
        }

        auto rendered_png = render_to_png(render_root,
            static_cast<uint32_t>(render_width),
            static_cast<uint32_t>(render_height), 2.0f);

        if (rendered_png.empty()) {
            std::cerr << "Validation error: headless render failed\n";
            return 1;
        }

        // Save rendered screenshot
        auto rendered_path = output_file + ".png";
        {
            std::ofstream f(rendered_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(rendered_png.data()),
                    static_cast<std::streamsize>(rendered_png.size()));
        }
        std::cout << "Rendered → " << rendered_path << " (" << render_width << "x" << render_height << ")\n";

        // Compare with reference if provided
        if (!reference_image.empty()) {
            auto result = compare_screenshot_files(reference_image, rendered_path);
            if (!result.valid) {
                std::cerr << "Comparison error: " << result.error << "\n";
                return 1;
            }

            std::cout << "Similarity: " << static_cast<int>(result.similarity * 100) << "% ("
                      << result.diff_pixels << "/" << result.total_pixels << " pixels differ, "
                      << "mean error: " << result.mean_error << ")\n";

            if (result.passes(0.70f)) {
                std::cout << "Validation: PASS\n";
            } else {
                std::cout << "Validation: NEEDS REVIEW (similarity below 70%)\n";
            }

            // Generate diff image if requested
            if (!diff_output.empty()) {
                auto ref_bytes = [&]() -> std::vector<uint8_t> {
                    std::ifstream f(reference_image, std::ios::binary);
                    return {std::istreambuf_iterator<char>(f), {}};
                }();
                auto diff_png = generate_diff_image(ref_bytes, rendered_png);
                if (!diff_png.empty()) {
                    std::ofstream f(diff_output, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(diff_png.data()),
                            static_cast<std::streamsize>(diff_png.size()));
                    std::cout << "Diff image → " << diff_output << "\n";
                }
            }
        }
    }

    // ── Debug JSON report ────────────────────────────────────────────────
    if (debug_json) {
        auto t_end = std::chrono::steady_clock::now();
        auto ms_total = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

        std::ostringstream dbg;
        dbg << "{\n";
        dbg << "  \"source\": \"" << design_source_name(*source) << "\",\n";
        dbg << "  \"input_file\": \"" << input_file << "\",\n";
        dbg << "  \"output_file\": \"" << output_file << "\",\n";
        dbg << "  \"mode\": \"" << (use_web_compat ? "web_compat" : "native") << "\",\n";
        dbg << "  \"elements\": {\n";
        dbg << "    \"total\": " << node_count << ",\n";
        dbg << "    \"containers\": " << container_count << ",\n";
        dbg << "    \"widgets\": " << widget_count << ",\n";
        dbg << "    \"labels\": " << text_count << "\n";
        dbg << "  },\n";
        dbg << "  \"tokens\": {\n";
        dbg << "    \"colors\": " << ir.tokens.colors.size() << ",\n";
        dbg << "    \"dimensions\": " << ir.tokens.dimensions.size() << ",\n";
        dbg << "    \"strings\": " << ir.tokens.strings.size() << "\n";
        dbg << "  },\n";
        dbg << "  \"timing_ms\": " << ms_total << ",\n";
        dbg << "  \"render_size\": \"" << render_width << "x" << render_height << "\",\n";
        dbg << "  \"js_bytes\": " << js.size() << ",\n";

        // Validation results if available
        if (validate && !reference_image.empty()) {
            auto result = compare_screenshot_files(reference_image, output_file + ".png");
            dbg << "  \"validation\": {\n";
            dbg << "    \"reference\": \"" << reference_image << "\",\n";
            dbg << "    \"similarity_pct\": " << static_cast<int>(result.similarity * 100) << ",\n";
            dbg << "    \"diff_pixels\": " << result.diff_pixels << ",\n";
            dbg << "    \"total_pixels\": " << result.total_pixels << ",\n";
            dbg << "    \"mean_error\": " << result.mean_error << ",\n";
            dbg << "    \"pass\": " << (result.passes(0.70f) ? "true" : "false") << "\n";
            dbg << "  },\n";
        }

        // List unprocessed/unsupported elements
        dbg << "  \"gaps\": [\n";
        bool first_gap = true;
        std::function<void(const IRNode&)> find_gaps = [&](const IRNode& n) {
            // Shapes that aren't audio widgets (not translated to Pulp widgets)
            if ((n.type == "ellipse" || n.type == "rectangle" || n.type == "path" ||
                 n.type == "polygon" || n.type == "line") &&
                n.audio_widget == AudioWidgetType::none) {
                if (!first_gap) dbg << ",\n";
                first_gap = false;
                dbg << "    {\"type\": \"" << n.type << "\", \"name\": \"" << n.name
                    << "\", \"reason\": \"shape not mapped to widget\"}";
            }
            for (auto& c : n.children) find_gaps(c);
        };
        find_gaps(ir.root);
        dbg << "\n  ]\n";

        dbg << "}\n";

        auto report = dbg.str();
        if (!debug_output.empty()) {
            write_file(debug_output, report);
            std::cout << "Debug report → " << debug_output << "\n";
        } else {
            std::cout << "\n" << report;
        }
    }

    return 0;
}
