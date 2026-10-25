// Inflates the compressed JS preludes embedded by core/view/js/embed_js.py.

#include <web_compat_preludes_gen.hpp>

#include <pulp/runtime/zip.hpp>

#include <stdexcept>

namespace pulp::view::preludes {

std::string source(const EmbeddedPrelude& prelude) {
    auto text = runtime::zlib_decompress_exact(prelude.deflated, prelude.deflated_size,
                                               prelude.source_size);
    if (!text) {
        throw std::runtime_error(std::string("embedded JS prelude is corrupt: ") +
                                 prelude.file_name);
    }
    return std::move(*text);
}

} // namespace pulp::view::preludes
