// Read the exact Rack 2 saved-patch format without extracting archive members.
// The caller sends one immutable .vcv byte snapshot on stdin; exactly one
// regular root patch.json is returned on stdout. Zstandard is the pinned,
// vendored decompressor in vendor/zstd-1.5.7, so runtime behavior never depends
// on PATH, Homebrew, Python extensions, tar, or the installed Rack application.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" {

struct ZSTD_DCtx_s;
using ZSTD_DStream = ZSTD_DCtx_s;
struct ZSTD_inBuffer {
    const void* src;
    std::size_t size;
    std::size_t pos;
};
struct ZSTD_outBuffer {
    void* dst;
    std::size_t size;
    std::size_t pos;
};

ZSTD_DStream* ZSTD_createDStream(void);
std::size_t ZSTD_freeDStream(ZSTD_DStream* stream);
std::size_t ZSTD_initDStream(ZSTD_DStream* stream);
std::size_t ZSTD_decompressStream(ZSTD_DStream* stream,
                                  ZSTD_outBuffer* output,
                                  ZSTD_inBuffer* input);
std::size_t ZSTD_DCtx_setParameter(ZSTD_DCtx_s* context,
                                   int parameter,
                                   int value);
unsigned ZSTD_isError(std::size_t code);
const char* ZSTD_getErrorName(std::size_t code);

}  // extern "C"

namespace {

constexpr std::size_t kMaxArchiveBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxTarBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kBlockBytes = 512;
constexpr int kWindowLogMaxParameter = 100;
constexpr int kWindowLogMax = 28;

class DecompressionStream {
  public:
    DecompressionStream() : stream_(ZSTD_createDStream()) {
        if (!stream_) throw std::runtime_error("could not allocate zstd decoder");
        check(ZSTD_DCtx_setParameter(
            stream_, kWindowLogMaxParameter, kWindowLogMax));
        check(ZSTD_initDStream(stream_));
    }

    ~DecompressionStream() { ZSTD_freeDStream(stream_); }
    ZSTD_DStream* get() const { return stream_; }

    static void check(std::size_t status) {
        if (ZSTD_isError(status))
            throw std::runtime_error(
                std::string("zstd decode failed: ") + ZSTD_getErrorName(status));
    }

  private:
    ZSTD_DStream* stream_;
};

std::vector<std::uint8_t> read_archive() {
    std::vector<std::uint8_t> bytes;
    std::array<char, 64 * 1024> block{};
    while (std::cin) {
        std::cin.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto count = std::cin.gcount();
        bytes.insert(bytes.end(), block.data(), block.data() + count);
        if (bytes.size() > kMaxArchiveBytes)
            throw std::runtime_error(
                "Rack patch archive exceeds the 256 MiB safety limit");
    }
    if (!std::cin.eof())
        throw std::runtime_error("could not read Rack patch bytes");
    if (bytes.empty()) throw std::runtime_error("Rack patch archive is empty");
    return bytes;
}

std::vector<std::uint8_t> decompress(
        const std::vector<std::uint8_t>& archive) {
    DecompressionStream decoder;
    ZSTD_inBuffer input{archive.data(), archive.size(), 0};
    std::vector<std::uint8_t> tar;
    std::array<std::uint8_t, 64 * 1024> block{};
    std::size_t remaining = 1;
    while (input.pos < input.size || remaining != 0) {
        const auto previous_input = input.pos;
        ZSTD_outBuffer output{block.data(), block.size(), 0};
        remaining = ZSTD_decompressStream(decoder.get(), &output, &input);
        DecompressionStream::check(remaining);
        if (output.pos > kMaxTarBytes - tar.size())
            throw std::runtime_error(
                "Rack patch expands beyond the 256 MiB safety limit");
        tar.insert(tar.end(), block.begin(), block.begin() + output.pos);
        if (input.pos == previous_input && output.pos == 0)
            throw std::runtime_error("zstd decoder made no progress");
        if (input.pos == input.size && remaining != 0 && output.pos == 0)
            throw std::runtime_error("Rack patch contains a truncated zstd frame");
    }
    return tar;
}

bool is_zero_block(const std::uint8_t* block) {
    return std::all_of(block, block + kBlockBytes,
                       [](std::uint8_t byte) { return byte == 0; });
}

std::uint64_t parse_octal(const std::uint8_t* field, std::size_t length,
                          std::string_view label) {
    std::size_t index = 0;
    while (index < length && field[index] == ' ') ++index;
    if (index == length || field[index] == 0)
        throw std::runtime_error(std::string(label) + " is empty");
    std::uint64_t value = 0;
    bool digit = false;
    for (; index < length; ++index) {
        const auto byte = field[index];
        if (byte == 0 || byte == ' ') break;
        if (byte < '0' || byte > '7')
            throw std::runtime_error(std::string(label) + " is not octal");
        digit = true;
        if (value > (std::numeric_limits<std::uint64_t>::max() >> 3))
            throw std::runtime_error(std::string(label) + " overflows");
        value = (value << 3) + (byte - '0');
    }
    if (!digit) throw std::runtime_error(std::string(label) + " is empty");
    for (; index < length; ++index) {
        if (field[index] != 0 && field[index] != ' ')
            throw std::runtime_error(
                std::string(label) + " has ambiguous trailing bytes");
    }
    return value;
}

std::string tar_text(const std::uint8_t* field, std::size_t length,
                     std::string_view label) {
    const auto end = std::find(field, field + length, 0);
    for (auto rest = end; rest != field + length; ++rest) {
        if (*rest != 0)
            throw std::runtime_error(
                std::string(label) + " has bytes after its terminator");
    }
    return std::string(reinterpret_cast<const char*>(field), end - field);
}

std::string safe_member_path(const std::uint8_t* header) {
    auto name = tar_text(header, 100, "tar member name");
    const auto prefix = tar_text(header + 345, 155, "tar member prefix");
    if (!prefix.empty()) name = prefix + "/" + name;
    if (name.empty() || name.front() == '/')
        throw std::runtime_error("tar member path is empty or absolute");
    if (name == "." || name == "./") return ".";

    std::string canonical;
    std::size_t begin = 0;
    while (begin <= name.size()) {
        const auto slash = name.find('/', begin);
        const auto end = slash == std::string::npos ? name.size() : slash;
        const auto component = name.substr(begin, end - begin);
        if (component == "..")
            throw std::runtime_error("tar member path traverses its root");
        if (component.empty() && end != name.size())
            throw std::runtime_error("tar member path has an empty component");
        if (!component.empty() && component != ".") {
            if (!canonical.empty()) canonical += '/';
            canonical += component;
        }
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    if (canonical.empty())
        throw std::runtime_error("tar member path is ambiguous");
    return canonical;
}

void validate_checksum(const std::uint8_t* header) {
    const auto declared = parse_octal(header + 148, 8, "tar checksum");
    std::uint64_t actual = 0;
    for (std::size_t index = 0; index < kBlockBytes; ++index)
        actual += (index >= 148 && index < 156) ? ' ' : header[index];
    if (actual != declared)
        throw std::runtime_error("tar header checksum does not match");
}

std::vector<std::uint8_t> read_patch_json(
        const std::vector<std::uint8_t>& tar) {
    std::vector<std::uint8_t> patch;
    bool found = false;
    bool terminated = false;
    std::size_t offset = 0;
    while (offset < tar.size()) {
        if (tar.size() - offset < kBlockBytes)
            throw std::runtime_error("tar ends inside a header block");
        const auto* header = tar.data() + offset;
        if (is_zero_block(header)) {
            if (tar.size() - offset < 2 * kBlockBytes ||
                    !is_zero_block(header + kBlockBytes))
                throw std::runtime_error("tar has only one end marker block");
            if (!std::all_of(header + 2 * kBlockBytes, tar.data() + tar.size(),
                             [](std::uint8_t byte) { return byte == 0; }))
                throw std::runtime_error("tar has ambiguous trailing data");
            terminated = true;
            break;
        }

        validate_checksum(header);
        const auto path = safe_member_path(header);
        const auto type = header[156];
        const auto declared = parse_octal(header + 124, 12, "tar member size");
        if (declared > kMaxTarBytes)
            throw std::runtime_error("tar member exceeds the safety limit");
        const auto size = static_cast<std::size_t>(declared);
        if (type == '5') {
            if (path != "." || size != 0)
                throw std::runtime_error(
                    "tar contains an unsupported directory member: " + path);
        } else if (type != 0 && type != '0') {
            throw std::runtime_error(
                "tar contains a non-regular member: " + path);
        }
        if ((type == 0 || type == '0') && path != "patch.json")
            throw std::runtime_error(
                "tar contains an unsupported regular member: " + path);
        const auto data_offset = offset + kBlockBytes;
        const auto padded = ((size + kBlockBytes - 1) / kBlockBytes) * kBlockBytes;
        if (padded > tar.size() - data_offset || size > tar.size() - data_offset)
            throw std::runtime_error("tar member extends beyond the archive");
        if (type == 0 || type == '0') {
            if (found) throw std::runtime_error("tar contains duplicate patch.json");
            found = true;
            patch.assign(tar.begin() + data_offset,
                         tar.begin() + data_offset + size);
        }
        offset = data_offset + padded;
    }
    if (!terminated) throw std::runtime_error("tar has no complete end marker");
    if (!found)
        throw std::runtime_error(
            "Rack patch archive contains no regular root patch.json");
    return patch;
}

}  // namespace

int main() {
    try {
        const auto archive = read_archive();
        const auto tar = decompress(archive);
        const auto patch = read_patch_json(tar);
        std::cout.write(reinterpret_cast<const char*>(patch.data()),
                        static_cast<std::streamsize>(patch.size()));
        if (!std::cout)
            throw std::runtime_error(
                "archived patch.json could not be copied completely");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
