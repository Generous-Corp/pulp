#include "control_protocol_seed_corpus.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pulp-generate-control-protocol-fuzz-corpus <output-directory>\n";
        return 2;
    }

    const std::filesystem::path output_directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) {
        std::cerr << "cannot create control protocol corpus directory: " << error.message() << '\n';
        return 1;
    }

    for (const auto& entry : pulp::test::control_protocol_fuzz::control_protocol_seed_corpus()) {
        if (entry.bytes.empty()) {
            std::cerr << "control protocol seed failed to encode: " << entry.filename << '\n';
            return 1;
        }
        std::ofstream output(output_directory / entry.filename, std::ios::binary | std::ios::trunc);
        if (!output ||
            !output.write(entry.bytes.data(), static_cast<std::streamsize>(entry.bytes.size()))) {
            std::cerr << "cannot write control protocol seed: " << entry.filename << '\n';
            return 1;
        }
    }
    return 0;
}
