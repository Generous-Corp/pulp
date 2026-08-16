#include "browser_import_cli.hpp"

#include "browser_capture_validation.hpp"
#include "browser_capture_workspace.hpp"
#include "browser_html_import.hpp"
#include "html_intake.hpp"
#include "browser_import_cli_internal.hpp"
#include "render_artifact_path.hpp"
#include "sprite_skins.hpp"

#include <pulp/view/design_sources.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pulp::import_design {

namespace fs = std::filesystem;

namespace {

std::string source_artifact_label(pulp::view::DesignSource source) {
    auto label = std::string(pulp::view::design_source_name(source));
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    std::replace_if(label.begin(), label.end(),
                    [](unsigned char c) {
                        return !std::isalnum(c) && c != '-' && c != '_';
                    },
                    '-');
    return label;
}

fs::path normalized_destination(const fs::path& path) {
    std::error_code ec;
    auto normalized = fs::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = fs::absolute(path, ec).lexically_normal();
    }
#if defined(__APPLE__) || defined(_WIN32)
    // The supported macOS and Windows filesystems are commonly
    // case-insensitive. Preflight runs before destinations exist, so
    // filesystem::equivalent cannot yet detect a mixed-case alias.
    auto identity = normalized.generic_string();
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return fs::path(identity);
#else
    return normalized;
#endif
}

bool validate_primary_output_destinations(
    const BrowserImportCliRequest& request,
    std::string& error) {
    std::vector<fs::path> outputs = request.reserved_output_paths;
    outputs.push_back(request.output_file);
    std::vector<fs::path> inputs{request.input_file};
    if (request.browser_interactions)
        inputs.push_back(*request.browser_interactions);
    if (!request.reference_image.empty())
        inputs.emplace_back(request.reference_image);
    for (const auto& output : outputs) {
        if (output.empty()) continue;
        const auto normalized_output = normalized_destination(output);
        for (const auto& input : inputs) {
            if (!input.empty() &&
                normalized_output == normalized_destination(input)) {
                error =
                    "browser primary output collides with a protected input";
                return false;
            }
        }
    }
    return true;
}

bool path_contains(const fs::path& directory, const fs::path& candidate) {
    const auto base = normalized_destination(directory);
    const auto path = normalized_destination(candidate);
    auto base_it = base.begin();
    auto path_it = path.begin();
    for (; base_it != base.end(); ++base_it, ++path_it) {
        if (path_it == path.end() || *base_it != *path_it) return false;
    }
    return true;
}

bool validate_publication_destinations(
    const BrowserImportCliRequest& request,
    const fs::path& durable_capture,
    const fs::path& rendered,
    const fs::path& diff,
    std::string& error) {
    const bool has_rendered = !rendered.empty();
    const bool has_diff = !diff.empty();
    const auto render = normalized_destination(rendered);
    const auto diff_path = normalized_destination(diff);
    std::vector<fs::path> protected_paths = request.reserved_output_paths;
    protected_paths.push_back(request.output_file);
    protected_paths.push_back(request.input_file);
    if (request.browser_interactions)
        protected_paths.push_back(*request.browser_interactions);
    if (!request.reference_image.empty())
        protected_paths.emplace_back(request.reference_image);
    if (std::any_of(
            protected_paths.begin(), protected_paths.end(),
            [&](const fs::path& protected_path) {
                if (protected_path.empty()) return false;
                const auto normalized =
                    normalized_destination(protected_path);
                return (has_rendered && render == normalized) ||
                       (has_diff && diff_path == normalized) ||
                       (!durable_capture.empty() &&
                        path_contains(durable_capture, normalized));
            })) {
        error = "browser validation artifact destination collides with "
                "a protected input or output, or durable capture would "
                "contain one";
        return false;
    }
    if (has_rendered && has_diff && render == diff_path) {
        error = "browser render and diff destinations must be distinct";
        return false;
    }
    if (!durable_capture.empty() &&
        ((has_rendered && path_contains(durable_capture, render)) ||
         (has_diff && path_contains(durable_capture, diff_path)))) {
        error = "browser validation artifact destination must not be inside "
                "the durable capture directory";
        return false;
    }
    return true;
}

std::vector<fs::path> localized_asset_paths(
    const pulp::view::DesignIR& ir,
    const fs::path& output_dir) {
    std::vector<fs::path> paths;
    const auto append = [&](std::string_view value) {
        if (value.empty()) return;
        fs::path path(value);
        if (path.is_relative()) path = output_dir / path;
        paths.push_back(std::move(path));
    };
    for (const auto& asset : ir.asset_manifest.assets)
        if (asset.local_path) append(*asset.local_path);
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            for (const char* key : {"fader_body_asset_path",
                                    "fader_indicator_asset_path"}) {
                if (const auto it = node.attributes.find(key);
                    it != node.attributes.end())
                    append(it->second);
            }
            for (const auto& alternate : node.alternate_frames) walk(alternate);
            for (const auto& child : node.children) walk(child);
        };
    walk(ir.root);
    return paths;
}

bool validate_localized_asset_destinations(
    const BrowserImportCliRequest& request,
    const pulp::view::DesignIR& ir,
    const fs::path& rendered,
    const fs::path& diff,
    std::string& error) {
    auto output_dir = request.output_file.parent_path();
    if (output_dir.empty()) output_dir = fs::current_path();
    const auto normalized_render = normalized_destination(rendered);
    const auto normalized_diff = normalized_destination(diff);
    std::vector<fs::path> protected_paths = request.reserved_output_paths;
    protected_paths.push_back(request.output_file);
    protected_paths.push_back(request.input_file);
    if (request.browser_interactions)
        protected_paths.push_back(*request.browser_interactions);
    if (!request.reference_image.empty())
        protected_paths.emplace_back(request.reference_image);
    for (const auto& asset_path : localized_asset_paths(ir, output_dir)) {
        const auto normalized_asset = normalized_destination(asset_path);
        if ((!rendered.empty() &&
             normalized_render == normalized_asset) ||
            (!diff.empty() &&
             normalized_diff == normalized_asset)) {
            error = "browser validation artifact destination collides with "
                    "a localized design asset";
            return false;
        }
        if (std::any_of(
                protected_paths.begin(), protected_paths.end(),
                [&](const fs::path& protected_path) {
                    return !protected_path.empty() &&
                           normalized_destination(protected_path) ==
                               normalized_asset;
                })) {
            error = "localized design asset destination collides with a "
                    "protected input or output";
            return false;
        }
    }
    return true;
}

struct ArtifactPublication {
    fs::path source;
    fs::path destination;
    bool check_protected_alias = false;
};

bool collect_staged_asset_publications(
    const fs::path& staging_root,
    const fs::path& output_file,
    std::vector<ArtifactPublication>& publications,
    std::string& error) {
    const auto staged_assets = staging_root / "assets";
    std::error_code ec;
    if (!fs::exists(staged_assets, ec)) return !ec;
    if (ec || !fs::is_directory(staged_assets, ec)) {
        error = "localized asset staging directory is invalid";
        return false;
    }
    auto output_directory = output_file.parent_path();
    if (output_directory.empty()) output_directory = fs::current_path(ec);
    if (ec) {
        error = "could not resolve localized asset output directory: " +
                ec.message();
        return false;
    }
    for (fs::directory_iterator it(staged_assets, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) {
            error = "localized asset staging contains a non-file entry";
            return false;
        }
        const auto destination =
            output_directory / "assets" / it->path().filename();
        if (fs::exists(destination, ec) && !ec) {
            std::ifstream staged(it->path(), std::ios::binary);
            std::ifstream existing(destination, std::ios::binary);
            if (!staged || !existing ||
                !std::equal(
                    std::istreambuf_iterator<char>(staged),
                    std::istreambuf_iterator<char>(),
                    std::istreambuf_iterator<char>(existing),
                    std::istreambuf_iterator<char>())) {
                error = "localized asset collision at " +
                        destination.string();
                return false;
            }
        } else if (ec) {
            error = "could not inspect localized asset destination: " +
                    ec.message();
            return false;
        }
        publications.push_back({it->path(), destination, false});
    }
    if (ec) {
        error = "could not enumerate localized asset staging: " +
                ec.message();
        return false;
    }
    return true;
}

}  // namespace

class BrowserCapturedImport::EvidenceTransaction {
public:
    EvidenceTransaction() = default;
    EvidenceTransaction(EvidenceTransaction&&) noexcept = default;
    EvidenceTransaction& operator=(EvidenceTransaction&&) noexcept = default;
    EvidenceTransaction(const EvidenceTransaction&) = delete;
    EvidenceTransaction& operator=(const EvidenceTransaction&) = delete;

    [[nodiscard]] bool commit(std::string& error);
    [[nodiscard]] fs::path stage_primary_output(
        const fs::path& destination, std::string& error);

    fs::path transient_capture;
    fs::path durable_capture;
    std::vector<ArtifactPublication> published_artifacts;
    std::vector<std::pair<fs::path, fs::path>> primary_artifacts;
    std::vector<fs::path> protected_paths;
    std::vector<std::shared_ptr<BrowserCaptureWorkspace>> workspaces;
    bool committed = false;
};

struct internal::BrowserImportCliResultBuilder {
    static BrowserCapturedImport make(
        pulp::view::DesignIR design_ir,
        int render_width,
        int render_height,
        bool similarity_failed,
        std::string reference_image,
        fs::path transient_capture,
        fs::path durable_capture,
        std::vector<ArtifactPublication> published_artifacts,
        std::vector<fs::path> protected_paths,
        std::vector<std::shared_ptr<BrowserCaptureWorkspace>> workspaces) {
        BrowserCapturedImport result;
        result.design_ir_ = std::move(design_ir);
        result.render_width_ = render_width;
        result.render_height_ = render_height;
        result.similarity_failed_ = similarity_failed;
        result.reference_image_ = std::move(reference_image);
        result.evidence_ =
            std::make_unique<BrowserCapturedImport::EvidenceTransaction>();
        result.evidence_->transient_capture = std::move(transient_capture);
        result.evidence_->durable_capture = std::move(durable_capture);
        result.evidence_->published_artifacts =
            std::move(published_artifacts);
        result.evidence_->protected_paths = std::move(protected_paths);
        result.evidence_->workspaces = std::move(workspaces);
        return result;
    }
};

BrowserCapturedImport::BrowserCapturedImport(
    BrowserCapturedImport&&) noexcept = default;
BrowserCapturedImport& BrowserCapturedImport::operator=(
    BrowserCapturedImport&&) noexcept = default;
BrowserCapturedImport::~BrowserCapturedImport() = default;

bool BrowserCapturedImport::commit_evidence(std::string& error) {
    return !evidence_ || evidence_->commit(error);
}

fs::path BrowserCapturedImport::stage_primary_output(
    const fs::path& destination, std::string& error) {
    return evidence_ ? evidence_->stage_primary_output(destination, error)
                     : destination;
}

fs::path BrowserCapturedImport::EvidenceTransaction::stage_primary_output(
    const fs::path& destination, std::string& error) {
    if (committed) {
        error = "browser output transaction is already committed";
        return {};
    }
    if (destination.empty()) {
        error = "browser primary output destination is empty";
        return {};
    }
    const auto identity = normalized_destination(destination);
    if (std::any_of(
            primary_artifacts.begin(), primary_artifacts.end(),
            [&](const auto& artifact) {
                return normalized_destination(artifact.second) == identity;
            })) {
        error = "browser primary output destination was staged twice: " +
                destination.string();
        return {};
    }
    std::error_code ec;
    const auto status = fs::symlink_status(destination, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "could not inspect browser primary output " +
                destination.string() + ": " + ec.message();
        return {};
    }
    if (!ec && status.type() != fs::file_type::not_found &&
        (fs::is_symlink(status) || !fs::is_regular_file(status))) {
        error = "browser primary output must be a regular file or absent: " +
                destination.string();
        return {};
    }
    const auto staging_root =
        transient_capture.parent_path() / "primary-output-staging";
    fs::create_directories(staging_root, ec);
    if (ec) {
        error = "could not create browser primary output staging directory: " +
                ec.message();
        return {};
    }
    auto filename = destination.filename();
    if (filename.empty()) filename = "output";
    const auto staged =
        staging_root /
        (std::to_string(primary_artifacts.size()) + "-" +
         filename.string());
    primary_artifacts.emplace_back(staged, destination);
    return staged;
}

bool BrowserCapturedImport::EvidenceTransaction::commit(
    std::string& error) {
    if (committed) return true;
    if (durable_capture.empty() && published_artifacts.empty() &&
        primary_artifacts.empty()) {
        committed = true;
        return true;
    }
    auto publications = published_artifacts;
    for (const auto& [source, destination] : primary_artifacts) {
        publications.push_back({source, destination, false});
    }

    struct PublishedFile {
        fs::path destination;
        fs::path staged;
        fs::path backup;
        bool had_destination = false;
        bool staged_created = false;
        bool backup_created = false;
        bool published = false;
    };
    std::vector<PublishedFile> files;
    files.reserve(publications.size());
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();

    // Outputs now exist, so supplement the preflight identity comparison with
    // the filesystem's own alias semantics before touching any destination.
    for (const auto& artifact : published_artifacts) {
        if (!artifact.check_protected_alias) continue;
        const auto& destination = artifact.destination;
        for (const auto& protected_path : protected_paths) {
            std::error_code equivalent_error;
            if (fs::exists(destination, equivalent_error) &&
                !equivalent_error &&
                fs::exists(protected_path, equivalent_error) &&
                !equivalent_error &&
                fs::equivalent(
                    destination, protected_path, equivalent_error) &&
                !equivalent_error) {
                error =
                    "browser validation artifact aliases a protected path";
                return false;
            }
        }
    }

    auto rollback_files = [&]() -> std::string {
        std::string rollback_error;
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            std::error_code ec;
            if (it->staged_created) {
                fs::remove(it->staged, ec);
                if (ec && rollback_error.empty())
                    rollback_error = ec.message();
            }
            ec.clear();
            if (it->published) {
                fs::remove(it->destination, ec);
                if (ec && rollback_error.empty())
                    rollback_error = ec.message();
            }
            ec.clear();
            if (it->backup_created) {
                fs::copy_file(
                    it->backup, it->destination,
                    fs::copy_options::none, ec);
                if (ec) {
                    if (rollback_error.empty()) {
                        rollback_error =
                            ec.message() + "; prior output retained at " +
                            it->backup.string();
                    }
                } else {
                    ec.clear();
                    fs::remove(it->backup, ec);
                    if (ec && rollback_error.empty())
                        rollback_error = ec.message();
                }
            }
        }
        return rollback_error;
    };
    auto append_rollback_error = [&](std::string rollback_error) {
        if (!rollback_error.empty())
            error += "; rollback failed: " + rollback_error;
    };

    for (const auto& publication : publications) {
        const auto& source = publication.source;
        const auto& destination = publication.destination;
        PublishedFile file;
        file.destination = destination;
        file.staged = destination;
        file.staged += ".tmp-" + std::to_string(nonce);
        file.backup = destination;
        file.backup += ".bak-" + std::to_string(nonce);

        std::error_code ec;
        const auto status = fs::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            error = "could not inspect browser transaction destination " +
                    destination.string() + ": " + ec.message();
            append_rollback_error(rollback_files());
            return false;
        }
        if (!ec && status.type() != fs::file_type::not_found &&
            (fs::is_symlink(status) || !fs::is_regular_file(status))) {
            error = "browser transaction destination must be a regular file "
                    "or absent: " + destination.string();
            append_rollback_error(rollback_files());
            return false;
        }
        ec.clear();
        if (!destination.parent_path().empty()) {
            fs::create_directories(destination.parent_path(), ec);
        }
        if (!ec) fs::copy_file(source, file.staged, fs::copy_options::none, ec);
        if (ec) {
            error = "could not stage browser validation artifact " +
                    destination.string() + ": " + ec.message();
            append_rollback_error(rollback_files());
            return false;
        }
        file.staged_created = true;
        file.had_destination = fs::exists(destination, ec);
        if (ec) {
            error = "could not re-inspect browser transaction destination " +
                    destination.string() + ": " + ec.message();
            files.push_back(std::move(file));
            append_rollback_error(rollback_files());
            return false;
        }
        if (file.had_destination) {
            fs::copy_file(
                destination, file.backup, fs::copy_options::none, ec);
            if (ec) {
                error = "could not back up browser validation artifact " +
                        destination.string() + ": " + ec.message();
                files.push_back(std::move(file));
                append_rollback_error(rollback_files());
                return false;
            }
            file.backup_created = true;
            fs::remove(destination, ec);
            if (ec) {
                error = "could not prepare browser validation artifact " +
                        destination.string() + ": " + ec.message();
                files.push_back(std::move(file));
                append_rollback_error(rollback_files());
                return false;
            }
        }
        fs::rename(file.staged, destination, ec);
        if (ec) {
            error = "could not publish browser validation artifact " +
                    destination.string() + ": " + ec.message();
            files.push_back(std::move(file));
            append_rollback_error(rollback_files());
            return false;
        }
        file.staged_created = false;
        file.published = true;
        files.push_back(std::move(file));
    }

    if (!durable_capture.empty() &&
        !commit_browser_capture_directory(
            transient_capture, durable_capture, error)) {
        append_rollback_error(rollback_files());
        return false;
    }

    std::error_code ec;
    for (const auto& file : files) {
        if (file.backup_created) fs::remove(file.backup, ec);
    }
    committed = true;
    return true;
}

BrowserImportCliResult internal::run_browser_import_cli_with_operations(
    const BrowserImportCliRequest& request,
    std::string_view content,
    const internal::BrowserImportCliOperations& operations) {
    std::string primary_output_error;
    if (!validate_primary_output_destinations(
            request, primary_output_error)) {
        std::cerr << "Error: " << primary_output_error << "\n";
        return BrowserImportFailure{2};
    }
    int render_width = request.initial_width;
    int render_height = request.initial_height;
    std::string reference_image = request.reference_image;
    const bool publish_validation_artifacts =
        request.validate || !request.reference_image.empty() ||
        !request.diff_output.empty();
    bool similarity_failed = false;
    pulp::view::DesignIR design_ir;
    fs::path transient_capture;
    fs::path durable_capture;
    std::vector<fs::path> protected_paths =
        request.reserved_output_paths;
    protected_paths.push_back(request.output_file);
    protected_paths.push_back(request.input_file);
    if (request.browser_interactions)
        protected_paths.push_back(*request.browser_interactions);
    if (!request.reference_image.empty())
        protected_paths.emplace_back(request.reference_image);

    auto browser_import = operations.import_html(
        {.input_file = request.input_file,
         .output_file = request.output_file,
         .importer_executable = request.importer_executable,
         .browser_executable = request.browser_executable,
         .browser_interactions = request.browser_interactions,
         .source = request.source,
         .initial_width = request.initial_width,
         .initial_height = request.initial_height,
         .offline = request.offline,
         .skia_validation =
             request.screenshot_backend ==
             pulp::view::ScreenshotBackend::skia,
         .allow_browser_network = request.allow_browser_network,
         .dry_run = request.dry_run,
         .supports_faithful_capture =
             request.supports_faithful_capture,
         .native_panel_lowering = request.native_panel_lowering,
         .materialized_canvas_composition =
             request.materialized_canvas_composition},
        content);

    std::vector<std::shared_ptr<BrowserCaptureWorkspace>> workspaces;
    if (auto* captured =
            std::get_if<BrowserHtmlCaptured>(&browser_import)) {
        workspaces = std::move(captured->workspaces);
        std::cout << "HTML intake: " << captured->shape
                  << " — evaluating with isolated Chromium\n";
        design_ir = std::move(captured->design_ir);
        transient_capture = captured->capture_directory;
        durable_capture =
            captured->durable_capture_directory;

        // --render-size is Chromium's initial responsive viewport. Validation
        // must use the settled portable canvas to avoid cropping tall or fixed
        // documents and reporting a false low similarity.
        // The canvas may only GROW the viewport. Its purpose is to stop a
        // tall or fixed document being cropped; a canvas SMALLER than the
        // captured viewport is a malformed lowering (a root narrower than its
        // own child), and adopting it silently rescales the reference onto a
        // smaller render, so every glyph and edge resamples and a faithful
        // design scores ~97%.
        if (design_ir.root.style.width &&
            design_ir.root.style.height) {
            render_width = std::max(
                render_width, static_cast<int>(
                       std::lround(*design_ir.root.style.width)));
            render_height = std::max(
                render_height, static_cast<int>(
                       std::lround(*design_ir.root.style.height)));
        }

        // The source browser pixels are intrinsic evidence. An explicit
        // --reference remains authoritative.
        if (reference_image.empty()) {
            reference_image =
                captured->reference_png.string();
        }

        std::cout << "Browser capture → "
                  << (durable_capture.empty()
                          ? captured->capture_directory
                          : durable_capture)
                  << (request.dry_run
                          ? " (transient; removed when dry-run exits)"
                          : "")
                  << "\n";
        std::cout << "Semantic report → semantic-report.json\n";
        // Before the visual-contract line, because a warning that a whole
        // class of content was not drawn changes what that contract means.
        for (const auto& warning : captured->warnings)
            std::cout << "Warning: " << warning << "\n";
        std::cout
            << "Visual contract: pixel-exact default frame; interactions are "
               "evidence-only and require a runtime bridge.\n";
    } else if (const auto* failure =
                   std::get_if<BrowserHtmlFailure>(&browser_import)) {
        workspaces = std::move(failure->workspaces);
        std::cerr << "Error: " << failure->error << "\n";
        return BrowserImportFailure{failure->exit_code};
    } else if (std::holds_alternative<BrowserHtmlLegacyFallback>(
                   browser_import)) {
        if (request.browser_interactions) {
            std::cerr
                << "Error: --browser-interactions requires browser-solved "
                   "runnable HTML and cannot be combined with --offline\n";
            return BrowserImportFailure{2};
        }
        std::cerr
            << "Warning: --offline selected the legacy partial HTML parser; "
               "CSS layout, runtime DOM, canvas/WebGL, and dynamic content may "
               "not match the browser.\n";
        return BrowserImportNotApplicable{};
    } else {
        if (request.browser_interactions) {
            std::cerr
                << "Error: --browser-interactions applies only to "
                   "browser-solved runnable HTML\n";
            return BrowserImportFailure{2};
        }
        if (request.fail_below_percent >= 0.0f &&
            reference_image.empty()) {
            std::cerr
                << "Error: --fail-below requires a successful browser "
                   "reference capture or --reference <png>\n";
            return BrowserImportFailure{2};
        }
        return BrowserImportNotApplicable{};
    }

    if (request.fail_below_percent >= 0.0f &&
        reference_image.empty()) {
        std::cerr
            << "Error: --fail-below requires a successful browser reference "
               "capture or --reference <png>\n";
        return BrowserImportFailure{2};
    }

    const auto design_name = request.output_file.stem().string();
    const auto source_label = source_artifact_label(request.source);
    const auto rendered_name =
        design_name + "-" + source_label + "-render.png";
    auto diff_name = request.diff_output.empty()
        ? std::string{}
        : fs::path(request.diff_output).filename().string();
    if (diff_name.empty()) {
        diff_name = design_name + "-" + source_label + "-diff.png";
    }
    const auto validation_staging =
        transient_capture / "validation-proof";
    const auto rendered_path =
        (validation_staging / "render/render.png").string();
    const auto diff_path =
        (validation_staging / "diff/diff.png").string();
    const auto published_rendered_path = render_artifact_path(
        request.output_file.string(), rendered_name);
    const auto published_diff_path = request.diff_output.empty()
        ? render_artifact_path(request.output_file.string(), diff_name)
        : request.diff_output;
    std::string destination_error;
    if (!validate_publication_destinations(
            request, durable_capture,
            publish_validation_artifacts
                ? fs::path(published_rendered_path)
                : fs::path{},
            publish_validation_artifacts
                ? fs::path(published_diff_path)
                : fs::path{},
            destination_error)) {
        std::cerr << "Error: " << destination_error << "\n";
        return BrowserImportFailure{2};
    }
    if (!request.dry_run &&
        !preflight_browser_capture_directory(
            durable_capture, destination_error)) {
        std::cerr << "Error: " << destination_error << "\n";
        return BrowserImportFailure{2};
    }
    std::vector<ArtifactPublication> published_artifacts;
    if (!request.dry_run && publish_validation_artifacts) {
        published_artifacts.push_back(
            {rendered_path, published_rendered_path, true});
        published_artifacts.push_back(
            {diff_path, published_diff_path, true});
    }

    // The reference must be registered against the render before it is scored.
    // That rect is resolved from the IR inside validate_capture, because the
    // registration has to hold for every caller of the comparison and not just
    // for the one that prints it. Deriving it here is what failed: the rect was
    // recovered from a faithful_capture child's negative offset, and native
    // panel lowering emits no such child, so the crop silently became zero and
    // every native panel was scored against a misregistered oracle.
    const auto comparison = operations.validate_capture(
        design_ir,
        {.reference = reference_image,
         .rendered = rendered_path,
         .diff = diff_path,
         .width = render_width,
         .height = render_height,
         .fail_below_percent = request.fail_below_percent,
         .backend = request.screenshot_backend});
    if (!comparison.valid) {
        std::cerr << "Validation error: " << comparison.error << "\n";
        return BrowserImportFailure{1};
    }

    // Native validation must see the capture's absolute transient paths.
    // Once those source pixels pass, localize the publishable IR beside the
    // requested output and fail closed if byte-identical portable assets
    // cannot be produced. Later emitter localization is then idempotent.
    std::string localization_error;
    if (!request.dry_run) {
        const auto localized_output_staging =
            transient_capture / "localized-output";
        const auto staged_output_file =
            localized_output_staging / request.output_file.filename();
        if (!operations.localize_assets(
                design_ir, staged_output_file.string(),
                &localization_error)) {
            std::cerr << "Error: " << localization_error << "\n";
            return BrowserImportFailure{1};
        }
        if (!validate_localized_asset_destinations(
                request, design_ir,
                publish_validation_artifacts
                    ? fs::path(published_rendered_path)
                    : fs::path{},
                publish_validation_artifacts
                    ? fs::path(published_diff_path)
                    : fs::path{},
                localization_error)) {
            std::cerr << "Error: " << localization_error << "\n";
            return BrowserImportFailure{2};
        }
        if (!collect_staged_asset_publications(
                localized_output_staging, request.output_file,
                published_artifacts, localization_error)) {
            std::cerr << "Error: " << localization_error << "\n";
            return BrowserImportFailure{2};
        }
        auto output_dir = request.output_file.parent_path();
        if (output_dir.empty()) output_dir = fs::current_path();
        auto asset_paths = localized_asset_paths(design_ir, output_dir);
        protected_paths.insert(protected_paths.end(),
                               std::make_move_iterator(asset_paths.begin()),
                               std::make_move_iterator(asset_paths.end()));
    }

    const auto retained_proof_root =
        request.dry_run
            ? validation_staging
            : durable_capture / "validation-proof";
    const auto reported_rendered_path =
        !request.dry_run && publish_validation_artifacts
            ? fs::path(published_rendered_path)
            : retained_proof_root / "render/render.png";
    const auto reported_diff_path =
        !request.dry_run && publish_validation_artifacts
            ? fs::path(published_diff_path)
            : retained_proof_root / "diff/diff.png";
    const auto proof_note =
        request.dry_run
            ? " (transient)"
            : publish_validation_artifacts
                ? ""
                : " (required capture evidence)";
    std::cout << "Rendered DesignIR → "
              << reported_rendered_path.string()
              << proof_note
              << " ("
              << render_width << "x" << render_height << ")\n";
    // A refusal prints where the number went instead of a number. Printing 0%
    // for a comparison that never ran is the same lie in the other direction,
    // and it is the one that gets quoted.
    if (comparison.scored) {
        std::cout << "Similarity: "
                  << static_cast<int>(comparison.similarity * 100) << "% ("
                  << comparison.diff_pixels << "/" << comparison.total_pixels
                  << " pixels differ, mean error: " << comparison.mean_error
                  << ")\n";
        std::cout << "Validation: "
                  << (comparison.passes ? "PASS" : "NEEDS REVIEW") << "\n";
    } else {
        std::cout << "Similarity: " << comparison.registration_reason << "\n";
        std::cout << "Validation: NEVER SCORED\n";
    }
    std::cout << "Diff image → "
              << reported_diff_path.string()
              << proof_note << "\n";
    // An unscorable comparison cannot satisfy a bar, so a run that ASKED for
    // one fails. A run that did not ask keeps its exit code: the panel imported
    // fine and only its oracle is unusable.
    if (request.fail_below_percent >= 0.0f &&
        !(comparison.scored && comparison.passes)) {
        similarity_failed = true;
    }

    return internal::BrowserImportCliResultBuilder::make(
        std::move(design_ir), render_width, render_height,
        similarity_failed, std::move(reference_image),
        std::move(transient_capture), std::move(durable_capture),
        std::move(published_artifacts),
        std::move(protected_paths),
        std::move(workspaces));
}

BrowserImportCliResult run_browser_import_cli(
    const BrowserImportCliRequest& request,
    std::string_view content) {
    const internal::BrowserImportCliOperations production_operations{
        .import_html = import_browser_html,
        .validate_capture = validate_browser_capture_design_ir,
        .localize_assets = localize_ir_assets,
    };
    return internal::run_browser_import_cli_with_operations(
        request, content, production_operations);
}

std::optional<int> run_browser_detect_cli(
    const fs::path& input_file,
    const std::optional<fs::path>& browser_executable) {
    std::ifstream stream(input_file, std::ios::binary);
    if (!stream) {
        std::cerr << "Error: cannot open " << input_file.string() << "\n";
        return 1;
    }
    std::ostringstream content;
    content << stream.rdbuf();
    const auto intake =
        classify_html_intake(input_file, content.str());
    if (!intake.use_browser) return std::nullopt;

    std::cout << "detected source: "
              << (intake.shape == HtmlExportShape::generic_html
                      ? "html" : "claude")
              << "\n";
    std::cout << "  format-version: "
              << html_export_shape_name(intake.shape) << "\n";
    std::cout << "  parser-version: browser-capture-v1\n";
    std::cout << "  evaluator: isolated Chromium\n";
    const auto readiness =
        probe_browser_import_readiness(browser_executable);
    if (readiness.available) {
        std::cout << "  browser: ready (" << readiness.product << " "
                  << readiness.version << ")\n";
    } else {
        std::cout << "  browser: unavailable\n";
        std::cout << readiness.error << "\n";
    }
    std::cout << "  confidence: 100%\n";
    return 0;
}

bool infer_browser_html_source_cli(
    const fs::path& input_file,
    std::string& source) {
    if (!source.empty() || input_file.empty()) return false;
    constexpr std::size_t kInferenceBytes = 512 * 1024;
    std::ifstream input(input_file, std::ios::binary);
    std::string prefix(kInferenceBytes, '\0');
    input.read(prefix.data(),
               static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(input.gcount()));
    const auto intake = classify_html_intake(input_file, prefix);
    if (!intake.use_browser) return false;
    source = intake.shape == HtmlExportShape::generic_html ? "html" : "claude";
    std::cout
        << "Detected runnable HTML; using the browser-solved import lane.\n";
    return true;
}

}  // namespace pulp::import_design
