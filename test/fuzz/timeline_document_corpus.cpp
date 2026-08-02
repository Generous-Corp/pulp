#include "timeline_document_corpus.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace pulp::test::timeline_fuzz {
namespace {

/// SplitMix64. Chosen because the whole corpus must be reconstructible from a
/// seed on any platform, which rules out the standard library's engines
/// (implementation-defined distributions) and anything drawing from the OS.
class Random {
  public:
    explicit Random(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// Returns a value in [0, bound). Returns 0 for an empty range.
    std::size_t below(std::size_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

  private:
    std::uint64_t state_ = 0;
};

/// Bytes that carry structural meaning to a JSON tokenizer, plus the encoding
/// edges (NUL, lone continuation byte, invalid lead byte) a UTF-8 validator
/// must reject.
constexpr std::array<char, 20> kInterestingBytes = {
    '"',  '\\', '{',  '}',  '[',  ']',  ':',  ',',  '0',  '9',
    '-',  '.',  'e',  ' ',  '\n', '\t', '\0', '\x7f', '\x80', '\xff',
};

/// Numeric spellings that sit on the edges of canonical-integer and
/// double-range validation.
constexpr std::array<std::string_view, 10> kNumericEdges = {
    "1e999",
    "-0",
    "9223372036854775808",
    "18446744073709551616",
    "0.0e-999999",
    "00",
    "1.",
    "-",
    "0x10",
    "1e-",
};

/// String payloads that exercise UTF-8 and escape decoding.
constexpr std::array<std::string_view, 6> kStringEdges = {
    R"("\uD800")",     // lone high surrogate
    R"("\uDC00")",     // lone low surrogate
    "\"\\u0000\"",  // escaped NUL
    R"("\q")",         // undefined escape
    "\"\xc0\xaf\"",    // overlong encoding
    R"("😀")", // valid astral pair
};

void flip_byte(std::string& bytes, Random& random) {
    if (bytes.empty()) {
        return;
    }
    const auto at = random.below(bytes.size());
    bytes[at] = kInterestingBytes[random.below(kInterestingBytes.size())];
}

void truncate(std::string& bytes, Random& random) {
    if (bytes.empty()) {
        return;
    }
    bytes.resize(random.below(bytes.size()));
}

/// Repeats an interior span. This is the operation that reaches the container
/// quotas: duplicating an array element run multiplies item counts without
/// changing the document's shape, which is exactly the growth the ceilings
/// exist to bound.
void duplicate_span(std::string& bytes, Random& random) {
    if (bytes.size() < 4) {
        return;
    }
    const auto begin = random.below(bytes.size() - 1);
    const auto length = 1 + random.below(std::min<std::size_t>(bytes.size() - begin, 64));
    const auto repeats = 1 + random.below(8);
    const std::string span = bytes.substr(begin, length);
    std::string repeated;
    repeated.reserve(span.size() * repeats);
    for (std::size_t i = 0; i < repeats; ++i) {
        repeated += span;
    }
    bytes.insert(begin, repeated);
}

/// Wraps the document in nested containers to drive it past the depth ceiling.
void nest(std::string& bytes, Random& random) {
    const auto depth = 1 + random.below(96);
    const char open = (random.next() & 1U) != 0 ? '[' : '{';
    const char close = open == '[' ? ']' : '}';
    std::string prefix(depth, open);
    std::string suffix(depth, close);
    bytes = prefix + bytes + suffix;
}

void splice_numeric(std::string& bytes, Random& random) {
    if (bytes.empty()) {
        return;
    }
    const auto edge = kNumericEdges[random.below(kNumericEdges.size())];
    const auto at = random.below(bytes.size());
    const auto length = random.below(std::min<std::size_t>(bytes.size() - at, 24));
    bytes.replace(at, length, edge);
}

void splice_string(std::string& bytes, Random& random) {
    if (bytes.empty()) {
        return;
    }
    const auto edge = kStringEdges[random.below(kStringEdges.size())];
    const auto at = random.below(bytes.size());
    const auto length = random.below(std::min<std::size_t>(bytes.size() - at, 24));
    bytes.replace(at, length, edge);
}

/// Finds the extent of the JSON value starting at `begin`, respecting strings
/// and escapes. Returns `npos` when the value does not close.
std::size_t value_end(const std::string& bytes, std::size_t begin) {
    const char open = bytes[begin];
    if (open != '[' && open != '{') {
        return std::string::npos;
    }
    const char close = open == '[' ? ']' : '}';
    std::size_t depth = 0;
    bool in_string = false;
    for (std::size_t at = begin; at < bytes.size(); ++at) {
        const char c = bytes[at];
        if (in_string) {
            if (c == '\\') {
                ++at;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == open) {
            ++depth;
        } else if (c == close) {
            if (--depth == 0) {
                return at + 1;
            }
        }
    }
    return std::string::npos;
}

/// Duplicates one complete, balanced element of a JSON array in place.
///
/// Byte-level mutation almost always breaks the document before the Timeline
/// model ever sees it, so nearly the whole budget is spent re-testing the
/// tokenizer. Repeating a syntactically complete element keeps the document
/// parseable and multiplies the item counts the structural quotas bound, which
/// is the growth this surface exists to refuse.
void duplicate_element(std::string& bytes, Random& random) {
    std::vector<std::size_t> starts;
    bool in_string = false;
    for (std::size_t at = 0; at + 1 < bytes.size(); ++at) {
        const char c = bytes[at];
        if (in_string) {
            if (c == '\\') {
                ++at;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if ((c == '[' || c == ',') && (bytes[at + 1] == '{' || bytes[at + 1] == '[')) {
            starts.push_back(at + 1);
        }
    }
    if (starts.empty()) {
        return;
    }

    const auto begin = starts[random.below(starts.size())];
    const auto end = value_end(bytes, begin);
    if (end == std::string::npos) {
        return;
    }
    const std::string element = bytes.substr(begin, end - begin);
    const auto repeats = 1 + random.below(4);
    std::string addition;
    addition.reserve((element.size() + 1) * repeats);
    for (std::size_t i = 0; i < repeats; ++i) {
        addition += ',';
        addition += element;
    }
    bytes.insert(end, addition);
}

void splice_seed(std::string& bytes, const std::string& other, Random& random) {
    if (bytes.empty() || other.empty()) {
        return;
    }
    const auto at = random.below(bytes.size());
    const auto take = random.below(other.size());
    bytes.insert(at, other.substr(0, take));
}

std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

std::vector<CorpusEntry> load_seed_corpus(const std::string& fixture_dir) {
    std::vector<CorpusEntry> corpus;
    std::ifstream index(fixture_dir + "/corpus.index");
    if (!index) {
        return corpus;
    }

    std::string line;
    while (std::getline(index, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string kind;
        std::string relative;
        if (!(fields >> kind >> relative)) {
            continue;
        }
        // `payload` blobs are declared never-decoded and `ignore` entries are
        // foreign formats; feeding either to the timeline parser would seed the
        // corpus with inputs whose rejection carries no information.
        if (kind != "document" && kind != "fragment") {
            continue;
        }
        auto bytes = read_file(fixture_dir + "/" + relative);
        if (bytes.empty()) {
            continue;
        }
        corpus.push_back(CorpusEntry{relative, std::move(bytes)});
    }
    return corpus;
}

CorpusMutator::CorpusMutator(std::vector<CorpusEntry> seeds, std::uint64_t seed)
    : seeds_(std::move(seeds)), seed_(seed) {}

CorpusEntry CorpusMutator::generate(std::uint64_t index) const {
    CorpusEntry entry;
    entry.label = "mutant/" + std::to_string(seed_) + "/" + std::to_string(index);
    if (seeds_.empty()) {
        return entry;
    }

    // Deriving the stream from (seed, index) rather than advancing one shared
    // stream is what makes any single case reproducible without replaying the
    // ones before it.
    Random random(seed_ ^ (index * 0x9E3779B97F4A7C15ULL));
    entry.bytes = seeds_[random.below(seeds_.size())].bytes;

    const auto rounds = 1 + random.below(4);
    for (std::size_t round = 0; round < rounds; ++round) {
        switch (random.below(10)) {
        case 0:
            flip_byte(entry.bytes, random);
            break;
        case 1:
            truncate(entry.bytes, random);
            break;
        case 2:
            duplicate_span(entry.bytes, random);
            break;
        case 3:
            nest(entry.bytes, random);
            break;
        case 4:
            splice_numeric(entry.bytes, random);
            break;
        case 5:
            splice_string(entry.bytes, random);
            break;
        case 6:
            splice_seed(entry.bytes, seeds_[random.below(seeds_.size())].bytes, random);
            break;
        default:
            // Weighted toward the structure-preserving operation: it is the one
            // that reaches past the tokenizer into the quotas under test.
            duplicate_element(entry.bytes, random);
            break;
        }
    }
    return entry;
}

} // namespace pulp::test::timeline_fuzz
