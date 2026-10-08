// SPDX-License-Identifier: MIT

#include "browser_capture_provenance.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace pulp::import_design::browser_capture {

namespace {
namespace fs = std::filesystem;
using choc::value::ValueView;
constexpr std::uint64_t kMaximumProvenanceFileBytes = 256ULL * 1024ULL * 1024ULL;

CaptureProvenanceResult fail(std::string message) {
    return {false, "browser-capture-provenance-invalid", std::move(message)};
}

ValueView object_member(const ValueView& object, const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) || !object[key].isObject())
        return {};
    return object[key];
}

std::optional<std::string> required_string(const ValueView& object, const char* key,
                                           bool allow_empty = false) {
    if (!object.isObject() || !object.hasObjectMember(key) || !object[key].isString())
        return std::nullopt;
    auto value = object[key].toString();
    if (!allow_empty && value.empty())
        return std::nullopt;
    return value;
}

std::optional<double> required_number(const ValueView& object, const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !(object[key].isInt() || object[key].isFloat()))
        return std::nullopt;
    const auto value = object[key].getWithDefault<double>(0.0);
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}

bool valid_sha256(std::string_view value) {
    if (value.size() != 64)
        return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool equal_hex(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i])))
            return false;
    }
    return true;
}

bool positive_integer(const ValueView& object, const char* key, int& value) {
    const auto parsed = required_number(object, key);
    if (!parsed || *parsed <= 0 || std::trunc(*parsed) != *parsed ||
        *parsed > std::numeric_limits<int>::max())
        return false;
    value = static_cast<int>(*parsed);
    return true;
}

bool nonnegative_integer(const ValueView& object, const char* key, int& value) {
    const auto parsed = required_number(object, key);
    if (!parsed || *parsed < 0 || std::trunc(*parsed) != *parsed ||
        *parsed > std::numeric_limits<int>::max())
        return false;
    value = static_cast<int>(*parsed);
    return true;
}

std::optional<fs::path> contained_path(const fs::path& envelope, std::string_view authored,
                                       std::string& error) {
    if (authored.empty()) {
        error = "materialized document path is empty";
        return std::nullopt;
    }
    const fs::path relative{authored};
    if (relative.is_absolute()) {
        error = "materialized document path must be relative";
        return std::nullopt;
    }
    std::error_code ec;
    const auto base = fs::weakly_canonical(envelope.parent_path(), ec);
    if (ec) {
        error = "could not resolve capture directory: " + ec.message();
        return std::nullopt;
    }
    const auto candidate = fs::weakly_canonical(base / relative, ec);
    if (ec) {
        error = "could not resolve materialized document: " + ec.message();
        return std::nullopt;
    }
    const auto rel = fs::relative(candidate, base, ec);
    if (ec || rel.empty() || rel.is_absolute() || *rel.begin() == fs::path("..")) {
        error = "materialized document escapes the capture directory";
        return std::nullopt;
    }
    if (!fs::is_regular_file(candidate, ec) || ec) {
        error = "materialized document is missing or not a regular file";
        return std::nullopt;
    }
    return candidate;
}
} // namespace

CaptureProvenanceResult validate_capture_provenance(const CaptureProvenanceRequest& request) {
    if (request.envelope.empty() || request.source.empty() || request.initial_width <= 0 ||
        request.initial_height <= 0 || request.device_scale_factor != kDefaultDeviceScaleFactor)
        return fail("capture provenance request has invalid paths, viewport, or DPR");

    const auto source_hash =
        pulp::runtime::sha256_file_hex(request.source, kMaximumProvenanceFileBytes);
    if (!source_hash)
        return fail("source file could not be hashed");

    std::ifstream input(request.envelope, std::ios::binary);
    if (!input)
        return fail("could not open capture envelope");
    input.seekg(0, std::ios::end);
    const auto envelope_size = input.tellg();
    if (envelope_size < 0 ||
        static_cast<std::uint64_t>(envelope_size) > kMaximumProvenanceFileBytes)
        return fail("capture envelope exceeds the provenance size limit");
    input.seekg(0, std::ios::beg);
    std::string envelope_bytes{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    choc::value::Value envelope;
    try {
        envelope = choc::json::parse(envelope_bytes);
    } catch (const std::exception& e) {
        return fail(std::string("invalid capture envelope JSON: ") + e.what());
    }
    if (!envelope.isObject() ||
        required_string(envelope, "schema").value_or("") != "pulp-browser-capture-v1" ||
        required_number(envelope, "version").value_or(0.0) != 1.0)
        return fail("unsupported capture envelope schema/version");

    const auto provenance = object_member(envelope, "provenance");
    if (required_string(provenance, "capture_method").value_or("") != "chromium-cdp")
        return fail("capture provenance method is missing or unsupported");

    const auto source = object_member(provenance, "source");
    const auto source_sha = required_string(source, "sha256");
    if (!source_sha || !valid_sha256(*source_sha))
        return fail("capture provenance is missing a valid source SHA-256");
    if (!equal_hex(*source_sha, *source_hash))
        return fail("capture source SHA-256 does not match the staged input");

    const auto browser = object_member(provenance, "browser");
    const auto browser_product = required_string(browser, "product");
    const auto browser_version = required_string(browser, "version");
    for (const char* key : {"protocol_version", "build_hash", "origin"})
        if (!required_string(browser, key))
            return fail(std::string("capture browser envelope is missing ") + key);
    if (!browser_product || !browser_version || *browser_product != request.browser.product ||
        *browser_version != request.browser.version)
        return fail("capture browser identity does not match the selected browser");

    const auto viewport = object_member(provenance, "viewport");
    const auto initial = object_member(viewport, "initial");
    const auto resolved = object_member(viewport, "resolved");
    const auto document = object_member(viewport, "document");
    int initial_width = 0, initial_height = 0, resolved_width = 0, resolved_height = 0,
        document_width = 0, document_height = 0;
    if (!positive_integer(initial, "width", initial_width) ||
        !positive_integer(initial, "height", initial_height) ||
        !positive_integer(resolved, "width", resolved_width) ||
        !positive_integer(resolved, "height", resolved_height) ||
        !positive_integer(document, "width", document_width) ||
        !positive_integer(document, "height", document_height))
        return fail("capture viewport envelope is missing positive dimensions");
    const auto dpr = required_number(viewport, "device_scale_factor");
    if (!dpr || *dpr != static_cast<double>(request.device_scale_factor))
        return fail("capture viewport DPR is missing or mismatched");
    if (initial_width != request.initial_width || initial_height != request.initial_height)
        return fail("capture initial viewport does not match the request");
    if (resolved_width > document_width || resolved_height > document_height)
        return fail("capture resolved viewport exceeds its document extent");

    const auto reference = object_member(envelope, "reference");
    const auto reference_dpr = required_number(reference, "device_scale_factor");
    const auto logical_width = required_number(reference, "logical_width");
    const auto logical_height = required_number(reference, "logical_height");
    if (!reference_dpr || *reference_dpr != *dpr || !logical_width || !logical_height ||
        *logical_width != document_width || *logical_height != document_height)
        return fail("capture reference viewport does not match provenance");

    const bool has_materialized_fields = source.hasObjectMember("materialized_document") ||
                                         source.hasObjectMember("materialized_document_sha256") ||
                                         source.hasObjectMember("materialized_asset_count");
    if (request.materialized_document.has_value()) {
        if (!has_materialized_fields)
            return fail("materialized document is present but unreferenced");
        const auto authored_path = required_string(source, "materialized_document");
        const auto authored_sha = required_string(source, "materialized_document_sha256");
        int authored_count = 0;
        if (!authored_path || !authored_sha || !valid_sha256(*authored_sha) ||
            !nonnegative_integer(source, "materialized_asset_count", authored_count))
            return fail("materialized document provenance is incomplete");
        std::string path_error;
        const auto contained = contained_path(request.envelope, *authored_path, path_error);
        if (!contained || fs::weakly_canonical(*contained) !=
                              fs::weakly_canonical(*request.materialized_document))
            return fail(path_error.empty() ? "materialized document path does not match capture"
                                           : path_error);
        const auto materialized_hash =
            pulp::runtime::sha256_file_hex(*contained, kMaximumProvenanceFileBytes);
        if (!materialized_hash || !equal_hex(*authored_sha, *materialized_hash))
            return fail("materialized document SHA-256 does not match its sidecar");
        std::ifstream materialized_input(*contained, std::ios::binary);
        std::string materialized_bytes{std::istreambuf_iterator<char>(materialized_input),
                                       std::istreambuf_iterator<char>()};
        choc::value::Value materialized;
        try {
            materialized = choc::json::parse(materialized_bytes);
        } catch (const std::exception& e) {
            return fail(std::string("invalid materialized document JSON: ") + e.what());
        }
        const auto schema = required_string(materialized, "schema").value_or("");
        const auto version = required_number(materialized, "version");
        const bool supported =
            (schema == "pulp-materialized-browser-document-v1" && version.value_or(0.0) == 1.0) ||
            (schema == "pulp-materialized-browser-document-v2" && version.value_or(0.0) == 2.0);
        if (!supported)
            return fail("unsupported materialized document schema/version");
        if (!required_string(materialized, "html") || !materialized.hasObjectMember("assets") ||
            !materialized["assets"].isArray() ||
            static_cast<int>(materialized["assets"].size()) != authored_count)
            return fail("materialized document metadata is incomplete or mismatched");
    } else if (has_materialized_fields) {
        return fail("capture references a materialized document that is absent");
    }

    return {true, {}, {}};
}

} // namespace pulp::import_design::browser_capture
