// pulp-js-bytecode: compiles a UI script to an embedded-bytecode blob at
// build time (see pulp::view::build_embedded_script_bytecode()).
//
//   pulp-js-bytecode [--widget-bridge-script] <input.js> <output.pjsbc>
//
// --widget-bridge-script compiles the script exactly as
// WidgetBridge::load_script() evaluates it. Built from the same Pulp libraries
// the product links, so the blob always carries that engine's fingerprint.
// Exit: 0 written, 1 the script does not compile, 2 usage or I/O error.

#include <pulp/view/js_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    bool widget_bridge = false;
    std::string input, output;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--widget-bridge-script")
            widget_bridge = true;
        else if (input.empty())
            input = arg;
        else if (output.empty())
            output = arg;
        else {
            std::fprintf(stderr, "pulp-js-bytecode: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        std::fprintf(stderr,
                     "usage: pulp-js-bytecode [--widget-bridge-script] <input.js> <output>\n");
        return 2;
    }

    std::ifstream in(input, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "pulp-js-bytecode: cannot read %s\n", input.c_str());
        return 2;
    }
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (widget_bridge)
        source = pulp::view::WidgetBridge::loaded_script_source(source);

    const auto blob = pulp::view::build_embedded_script_bytecode(source);
    if (blob.empty()) {
        std::fprintf(stderr, "pulp-js-bytecode: %s does not compile\n", input.c_str());
        return 1;
    }

    // Write beside the target and rename, so an interrupted build never
    // leaves a truncated blob that a later incremental build would embed.
    const std::string partial = output + ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        if (!out) {
            std::fprintf(stderr, "pulp-js-bytecode: cannot write %s\n", partial.c_str());
            return 2;
        }
    }
    if (std::rename(partial.c_str(), output.c_str()) != 0) {
        std::remove(output.c_str());
        if (std::rename(partial.c_str(), output.c_str()) != 0) {
            std::fprintf(stderr, "pulp-js-bytecode: cannot write %s\n", output.c_str());
            return 2;
        }
    }
    return 0;
}
