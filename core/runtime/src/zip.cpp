#include <pulp/runtime/zip.hpp>
#include <miniz.h>
#include <cstring>

namespace pulp::runtime {

std::optional<std::vector<uint8_t>> gzip_compress(const uint8_t* data, size_t size, int level) {
    // Estimate compressed size
    mz_ulong comp_size = mz_compressBound(static_cast<mz_ulong>(size));
    std::vector<uint8_t> result(comp_size);

    int status = mz_compress2(result.data(), &comp_size, data,
                              static_cast<mz_ulong>(size), level);
    if (status != MZ_OK)
        return std::nullopt;

    result.resize(comp_size);
    return result;
}

std::optional<std::vector<uint8_t>> gzip_decompress(const uint8_t* data, size_t size) {
    // Start with 4x the input size, grow if needed
    mz_ulong decomp_size = static_cast<mz_ulong>(size * 4);
    std::vector<uint8_t> result(decomp_size);

    for (int attempt = 0; attempt < 5; ++attempt) {
        int status = mz_uncompress(result.data(), &decomp_size, data,
                                   static_cast<mz_ulong>(size));
        if (status == MZ_OK) {
            result.resize(decomp_size);
            return result;
        }
        if (status == MZ_BUF_ERROR) {
            decomp_size *= 2;
            result.resize(decomp_size);
            continue;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> gzip_compress(std::string_view data, int level) {
    return gzip_compress(reinterpret_cast<const uint8_t*>(data.data()), data.size(), level);
}

std::optional<std::string> gzip_decompress_string(const uint8_t* data, size_t size) {
    auto result = gzip_decompress(data, size);
    if (!result) return std::nullopt;
    return std::string(result->begin(), result->end());
}

std::optional<std::vector<uint8_t>> deflate_compress(const uint8_t* data, size_t size, int level) {
    mz_ulong comp_size = mz_deflateBound(nullptr, static_cast<mz_ulong>(size));
    std::vector<uint8_t> result(comp_size);

    mz_stream stream{};
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(size);
    stream.next_out = result.data();
    stream.avail_out = static_cast<unsigned int>(result.size());

    if (mz_deflateInit2(&stream, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS,
                        9, MZ_DEFAULT_STRATEGY) != MZ_OK)
        return std::nullopt;

    int status = mz_deflate(&stream, MZ_FINISH);
    mz_deflateEnd(&stream);

    if (status != MZ_STREAM_END)
        return std::nullopt;

    result.resize(stream.total_out);
    return result;
}

std::optional<std::vector<uint8_t>> deflate_decompress(const uint8_t* data, size_t size) {
    std::vector<uint8_t> result;
    result.resize(size * 4);

    mz_stream stream{};
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(size);
    stream.next_out = result.data();
    stream.avail_out = static_cast<unsigned int>(result.size());

    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return std::nullopt;

    for (int attempt = 0; attempt < 10; ++attempt) {
        int status = mz_inflate(&stream, MZ_FINISH);
        if (status == MZ_STREAM_END) {
            result.resize(stream.total_out);
            mz_inflateEnd(&stream);
            return result;
        }
        if (status == MZ_BUF_ERROR) {
            size_t old_size = result.size();
            result.resize(old_size * 2);
            stream.next_out = result.data() + stream.total_out;
            stream.avail_out = static_cast<unsigned int>(result.size() - stream.total_out);
            continue;
        }
        mz_inflateEnd(&stream);
        return std::nullopt;
    }

    mz_inflateEnd(&stream);
    return std::nullopt;
}

}  // namespace pulp::runtime
