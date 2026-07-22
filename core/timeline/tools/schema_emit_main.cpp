// Standalone emitter for the timeline schema manifest.
//
// Builds the built-in timeline schema registry and writes its canonical
// JSON-Schema projection to stdout (or --out <file>). This is the generator
// half of the schema-drift gate: `schema_drift_check.py` runs this binary and
// compares the output against the committed artifact.

#include <pulp/timeline/schema_codegen.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    const char* out_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::fputs("usage: pulp-timeline-schema-emit [--out <file>]\n", stdout);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry) {
        std::fprintf(stderr, "failed to build built-in timeline registry (schema error %u for %s)\n",
                     static_cast<unsigned>(registry.error().code),
                     registry.error().type_name.c_str());
        return 1;
    }

    auto manifest = pulp::timeline::emit_schema_manifest(registry.value());
    if (!manifest) {
        std::fprintf(stderr, "failed to emit schema manifest (persistence error %u)\n",
                     static_cast<unsigned>(manifest.error().code));
        return 1;
    }

    const std::string& bytes = manifest.value();
    FILE* out = stdout;
    if (out_path != nullptr) {
        out = std::fopen(out_path, "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "cannot open %s for writing\n", out_path);
            return 1;
        }
    }
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), out);
    // Trailing newline so the committed file is POSIX-friendly and byte-stable.
    if (written == bytes.size())
        std::fputc('\n', out);
    if (out_path != nullptr)
        std::fclose(out);
    if (written != bytes.size()) {
        std::fprintf(stderr, "short write\n");
        return 1;
    }
    return 0;
}
