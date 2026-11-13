#include "recorder_core.hpp"

#include <pulp/runtime/crypto.hpp>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>

namespace pulp::testing {

namespace {

struct State {
    std::mutex mutex;
    std::atomic<bool> recording{false};
    TextureUploadRecorder::Options options;
    std::vector<TextureUploadRecord> records;
    std::ofstream manifest;
};

State& state() {
    static State s;
    return s;
}

bool inside_git_work_tree(const std::filesystem::path& dir) {
    std::error_code ec;
    auto at = std::filesystem::weakly_canonical(std::filesystem::absolute(dir, ec), ec);
    while (!at.empty()) {
        if (std::filesystem::exists(at / ".git", ec))
            return true;
        const auto parent = at.parent_path();
        if (parent == at)
            break;
        at = parent;
    }
    return false;
}

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

std::string manifest_line(const TextureUploadRecord& r) {
    std::string line = "{";
    auto field = [&](const char* name) {
        if (line.size() > 1)
            line += ',';
        append_json_string(line, name);
        line += ':';
    };
    auto number = [&](const char* name, std::uint64_t value) {
        field(name);
        line += std::to_string(value);
    };
    auto text = [&](const char* name, const std::string& value) {
        field(name);
        append_json_string(line, value);
    };
    number("seq", r.seq);
    text("api", r.api);
    text("entry", r.entry);
    number("texture_id", r.texture_id);
    number("width", r.width);
    number("height", r.height);
    number("depth", r.depth);
    number("x", r.x);
    number("y", r.y);
    number("z", r.z);
    number("mip", r.mip);
    number("slice", r.slice);
    text("pixel_format", r.pixel_format);
    number("pixel_format_value", r.pixel_format_value);
    number("source_bytes_per_row", r.source_bytes_per_row);
    field("padding_stripped");
    line += r.padding_stripped ? "true" : "false";
    number("byte_length", r.byte_length);
    text("sha256", r.sha256);
    text("raw_file", r.raw_file);
    text("caller_image", r.caller_image);
    line += "}\n";
    return line;
}

} // namespace

bool TextureUploadRecorder::install(const Options& options, std::string* error) {
    auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.recording.load())
        return fail("already recording; call stop() first");
    if (options.output_dir.empty())
        return fail("output_dir is required");
    // Checked before anything is created, so a refusal leaves no trace.
    if (!options.allow_inside_repository && inside_git_work_tree(options.output_dir))
        return fail(options.output_dir.string() +
                    " is inside a git working tree; recorded textures can be a product's art, so "
                    "choose a directory outside any repository or set allow_inside_repository");
    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    if (ec || !std::filesystem::is_directory(options.output_dir))
        return fail("cannot create " + options.output_dir.string());
    if (!detail::install_platform_hooks(error))
        return false;
    s.manifest.close();
    s.manifest.clear();
    s.manifest.open(options.output_dir / "manifest.jsonl", std::ios::binary | std::ios::trunc);
    if (!s.manifest)
        return fail("cannot write " + (options.output_dir / "manifest.jsonl").string());
    s.options = options;
    s.records.clear();
    s.recording.store(true);
    return true;
}

void TextureUploadRecorder::stop() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.recording.store(false);
    s.manifest.close();
}

bool TextureUploadRecorder::recording() {
    return state().recording.load();
}

std::vector<TextureUploadRecord> TextureUploadRecorder::records() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.records;
}

namespace detail {

bool recording() {
    return state().recording.load(std::memory_order_relaxed);
}

bool pack_rows(const UploadDescription& u, std::string& out) {
    out.clear();
    if (!u.data)
        return false;
    const std::uint64_t height = u.height ? u.height : 1;
    const std::uint64_t depth = u.depth ? u.depth : 1;
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(u.width) * u.bytes_per_pixel;
    const std::uint64_t source_row = u.source_bytes_per_row ? u.source_bytes_per_row : row_bytes;
    const std::uint64_t source_image =
        u.source_bytes_per_image ? u.source_bytes_per_image : source_row * height;
    const auto* bytes = reinterpret_cast<const char*>(u.data);
    if (u.bytes_per_pixel == 0 || row_bytes > source_row) {
        // Unknown pixel size: keep the rows as given, padding included.
        for (std::uint64_t z = 0; z < depth; ++z)
            out.append(bytes + z * source_image, static_cast<std::size_t>(source_row * height));
        return false;
    }
    out.reserve(static_cast<std::size_t>(row_bytes * height * depth));
    for (std::uint64_t z = 0; z < depth; ++z)
        for (std::uint64_t y = 0; y < height; ++y)
            out.append(bytes + z * source_image + y * source_row,
                       static_cast<std::size_t>(row_bytes));
    return true;
}

void record_upload(const UploadDescription& u) {
    auto& s = state();
    if (!s.recording.load())
        return;
    std::string packed;
    const bool stripped = pack_rows(u, packed);

    std::lock_guard lock(s.mutex);
    if (!s.recording.load())
        return;
    TextureUploadRecord r;
    r.seq = s.records.size() + 1;
    r.api = u.api;
    r.entry = u.entry;
    r.texture_id = u.texture_id;
    r.width = u.width;
    r.height = u.height;
    r.depth = u.depth;
    r.x = u.x;
    r.y = u.y;
    r.z = u.z;
    r.mip = u.mip;
    r.slice = u.slice;
    r.pixel_format = u.pixel_format;
    r.pixel_format_value = u.pixel_format_value;
    r.source_bytes_per_row = u.source_bytes_per_row;
    r.padding_stripped = stripped;
    r.caller_image = u.caller_image;
    if (u.data) {
        r.byte_length = packed.size();
        r.sha256 = runtime::sha256_hex(packed);
        if (s.options.write_raw) {
            char name[64];
            std::snprintf(name, sizeof name, "%06llu-%.12s.bin",
                          static_cast<unsigned long long>(r.seq), r.sha256.c_str());
            std::ofstream raw(s.options.output_dir / name, std::ios::binary | std::ios::trunc);
            raw.write(packed.data(), static_cast<std::streamsize>(packed.size()));
            if (raw)
                r.raw_file = name;
        }
    }
    s.manifest << manifest_line(r);
    s.manifest.flush();
    s.records.push_back(std::move(r));
}

} // namespace detail

} // namespace pulp::testing
