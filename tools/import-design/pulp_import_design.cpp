#include <pulp/view/design_import.hpp>
#include <pulp/view/design_export.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace pulp::view;

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
    std::cout << "  --output <path>   Output JS file (default: ui.js)\n";
    std::cout << "  --tokens <path>   Output W3C token file (default: tokens.json)\n";
    std::cout << "  --dry-run         Show generated code without writing files\n";
    std::cout << "  --no-tokens       Skip token extraction\n";
    std::cout << "  --no-comments     Omit comments from generated code\n";
    std::cout << "  --help            Show this help\n\n";
    std::cout << "Examples:\n";
    std::cout << "  pulp import-design --from figma --file design.json\n";
    std::cout << "  pulp import-design --from v0 --file component.tsx --output my-ui.js\n";
    std::cout << "  pulp import-design --from pencil --file design.json --dry-run\n";
    std::cout << "  pulp import-design --from stitch --file screen.html --tokens theme.json\n";
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
    std::string output_file = "ui.js";
    std::string tokens_file = "tokens.json";
    std::string export_format = "w3c";
    bool dry_run = false;
    bool include_tokens = true;
    bool include_comments = true;
    bool export_tokens_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            source_str = argv[++i];
        } else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            input_file = argv[++i];
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

    if (input_file.empty()) {
        std::cerr << "Error: --file <path> is required\n";
        return 1;
    }

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
    ir.source_file = input_file;

    // Generate Pulp JS
    CodeGenOptions opts;
    opts.include_tokens = include_tokens;
    opts.include_comments = include_comments;
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

    // Write output files
    if (!write_file(output_file, js)) return 1;

    size_t node_count = 0;
    std::function<void(const IRNode&)> count_nodes = [&](const IRNode& n) {
        node_count++;
        for (auto& c : n.children) count_nodes(c);
    };
    count_nodes(ir.root);

    std::cout << "Wrote " << output_file << " (" << node_count << " elements";

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
    return 0;
}
