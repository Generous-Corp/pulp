// Build-time generator for the emitter conformance gate: emits one fixture's
// baked C++ into the build tree so the gate compiles and runs exactly what
// generate_pulp_cpp() produces today. Nothing generated is committed, so the
// C++ lane can never be compared from a stale copy.
//
//   pulp-emitter-conformance-gen <ir|figma-plugin> <input.json> <namespace> <out-dir> <stem>
//
// writes <out-dir>/<stem>.hpp, <out-dir>/<stem>.cpp and <out-dir>/<stem>_entry.cpp.
// The entry file gives every fixture the same two functions in its namespace,
// conformance_build() and conformance_bind(), because the exporter only emits
// a binding function for a design that has bindings.

#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/design_sources.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

bool write_if_changed(const fs::path& path, const std::string& text) {
    std::ifstream existing(path, std::ios::binary);
    if (existing) {
        const std::string current((std::istreambuf_iterator<char>(existing)),
                                  std::istreambuf_iterator<char>());
        if (current == text)
            return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: pulp-emitter-conformance-gen <ir|figma-plugin> <input.json> "
                     "<namespace> <out-dir> <stem>\n";
        return 2;
    }
    const std::string kind = argv[1];
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) {
        std::cerr << "cannot read " << argv[2] << "\n";
        return 1;
    }
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    pulp::view::DesignIR ir;
    try {
        ir = kind == "figma-plugin" ? pulp::view::parse_figma_plugin_json(json)
                                    : pulp::view::parse_design_ir_json(json);
    } catch (const std::exception& error) {
        std::cerr << "cannot parse " << argv[2] << ": " << error.what() << "\n";
        return 1;
    }

    const std::string stem = argv[5];
    pulp::view::CppExportOptions options;
    options.namespace_name = argv[3];
    options.header_filename = stem + ".hpp";
    const auto generated = pulp::view::generate_pulp_cpp(ir, ir.asset_manifest, options);

    const fs::path out_dir = argv[4];
    fs::create_directories(out_dir);
    const bool has_binder =
        generated.header.find(options.binding_function_name + "(") != std::string::npos;
    std::string entry = "#include \"" + stem + ".hpp\"\n\n";
    entry += "namespace " + options.namespace_name + " {\n\n";
    entry += "std::unique_ptr<pulp::view::View> conformance_build() { return " +
             options.function_name + "(); }\n\n";
    entry += "void conformance_bind(pulp::view::View& root, "
             "pulp::view::NativeImportBindingContext& ctx) {\n";
    entry += has_binder ? "    " + options.binding_function_name + "(root, ctx);\n"
                        : "    (void)root;\n    (void)ctx;\n";
    entry += "}\n\n} // namespace " + options.namespace_name + "\n";
    if (!write_if_changed(out_dir / (stem + ".hpp"), generated.header) ||
        !write_if_changed(out_dir / (stem + ".cpp"), generated.source) ||
        !write_if_changed(out_dir / (stem + "_entry.cpp"), entry)) {
        std::cerr << "cannot write generated sources under " << out_dir << "\n";
        return 1;
    }
    return 0;
}
