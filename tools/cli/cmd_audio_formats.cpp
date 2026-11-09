// cmd_audio_formats.cpp — pulp audio formats

#include "cmd_audio_formats.hpp"

#include <pulp/audio/format_registry.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using pulp::audio::FormatCapabilities;

void print_usage(std::ostream& out) {
    out << "Usage: pulp audio formats [--json]\n\n"
           "Lists the audio file readers and writers this build registers, with\n"
           "their extensions, bit depths, sample rates, channel limits and encoder\n"
           "settings. An empty list means the format does not restrict that value.\n";
}

choc::value::Value strings_value(const std::vector<std::string>& items) {
    auto array = choc::value::createEmptyArray();
    for (const auto& item : items)
        array.addArrayElement(item);
    return array;
}

choc::value::Value numbers_value(const std::vector<std::uint32_t>& items) {
    auto array = choc::value::createEmptyArray();
    for (auto item : items)
        array.addArrayElement(static_cast<std::int64_t>(item));
    return array;
}

choc::value::Value capabilities_value(const FormatCapabilities& caps) {
    auto object = choc::value::createObject("");
    object.addMember("name", caps.format_name);
    object.addMember("extensions", strings_value(caps.extensions));
    object.addMember("bit_depths", numbers_value(caps.bit_depths));
    object.addMember("float_samples", caps.float_samples);
    object.addMember("sample_rates", numbers_value(caps.sample_rates));
    object.addMember("max_channels", static_cast<std::int64_t>(caps.max_channels));
    object.addMember("compressed", caps.compressed);
    object.addMember("lossless", caps.lossless);
    auto options = choc::value::createEmptyArray();
    for (const auto& option : caps.quality_options) {
        auto entry = choc::value::createObject("");
        entry.addMember("name", option.name);
        entry.addMember("description", option.description);
        entry.addMember("default", option.default_value);
        options.addArrayElement(entry);
    }
    object.addMember("quality_options", options);
    return object;
}

choc::value::Value list_value(const std::vector<FormatCapabilities>& list) {
    auto array = choc::value::createEmptyArray();
    for (const auto& caps : list)
        array.addArrayElement(capabilities_value(caps));
    return array;
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty())
            out += " ";
        out += item;
    }
    return out;
}

std::string join(const std::vector<std::uint32_t>& items, const char* empty) {
    if (items.empty())
        return empty;
    std::string out;
    for (auto item : items) {
        if (!out.empty())
            out += ",";
        out += std::to_string(item);
    }
    return out;
}

void print_table(const char* title, const std::vector<FormatCapabilities>& list) {
    std::cout << title << "\n";
    for (const auto& caps : list) {
        std::cout << "  " << caps.format_name << "  [" << join(caps.extensions) << "]\n";
        std::cout << "    " << (caps.compressed ? "compressed" : "uncompressed") << ", "
                  << (caps.lossless ? "lossless" : "lossy")
                  << "; bit depths: " << join(caps.bit_depths, "n/a")
                  << (caps.float_samples ? " +float" : "")
                  << "; sample rates: " << join(caps.sample_rates, "any") << "; channels: "
                  << (caps.max_channels == 0 ? std::string("any")
                                             : "up to " + std::to_string(caps.max_channels))
                  << "\n";
        for (const auto& option : caps.quality_options)
            std::cout << "    " << option.name << " = " << option.default_value << "  ("
                      << option.description << ")\n";
    }
}

} // namespace

int cmd_audio_formats(const std::vector<std::string>& args) {
    bool json_output = false;
    for (const auto& arg : args) {
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(std::cerr);
            return 1;
        }
    }

    const auto& registry = pulp::audio::FormatRegistry::instance();
    const auto readers = registry.reader_capabilities();
    const auto writers = registry.writer_capabilities();

    if (json_output) {
        auto root = choc::value::createObject("");
        root.addMember("readers", list_value(readers));
        root.addMember("writers", list_value(writers));
        std::cout << choc::json::toString(root, true) << "\n";
        return 0;
    }

    print_table("Readers", readers);
    std::cout << "\n";
    print_table("Writers", writers);
    return 0;
}
