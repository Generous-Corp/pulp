#include "browser_capture_validation.hpp"
#include "import_png_codec.hpp"

#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <chrono>
#include <charconv>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace pulp::import_design {

namespace {

bool write_bytes_atomically(
    const std::filesystem::path& destination,
    const std::vector<std::uint8_t>& bytes,
    std::string& error) {
    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(
            destination.parent_path(), ec);
    }
    if (ec) {
        error = "could not create validation artifact directory " +
                destination.parent_path().string() + ": " + ec.message();
        return false;
    }
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    auto staged = destination;
    staged += ".tmp-" + std::to_string(nonce);
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            error = "could not write validation artifact " +
                    destination.string();
            std::error_code ec;
            std::filesystem::remove(staged, ec);
            return false;
        }
    }
    ec.clear();
    std::filesystem::rename(staged, destination, ec);
    if (ec) {
        std::filesystem::remove(staged, ec);
        error = "could not commit validation artifact " +
                destination.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::optional<double> attribute_number(
    const pulp::view::IRNode& node, const char* key) {
    const auto found = node.attributes.find(key);
    if (found == node.attributes.end()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const double value = std::stod(found->second, &consumed);
        if (consumed != found->second.size() || !std::isfinite(value))
            return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::uint64_t> attribute_uint64(
    const pulp::view::IRNode& node, const char* key) {
    const auto found = node.attributes.find(key);
    if (found == node.attributes.end() || found->second.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    const auto* begin = found->second.data();
    const auto* end = begin + found->second.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || parsed_end != end) return std::nullopt;
    return value;
}

std::string extent(int width, int height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

bool compose_materialized_canvas_evidence(
    const pulp::view::DesignIR& ir,
    std::vector<std::uint8_t>& rendered,
    std::string& error) {
    const auto authority = ir.root.attributes.find(
        "materialized_visual_authority");
    if (authority == ir.root.attributes.end() ||
        authority->second != "browser:chrome+native-canvases")
        return true;

    const auto native_render = decode_png_rgba(rendered.data(), rendered.size());
    if (!native_render.valid()) {
        error = "could not decode materialized validation render";
        return false;
    }

    // The accepted chrome plate is already a DPR-exact Chromium raster. A
    // decode -> Skia ImageView -> encode round trip can legitimately perturb
    // premultiplication/color rounding by one byte, which makes an exact-pixel
    // browser authority gate fail even when geometry and paint are unchanged.
    // Compose evidence from the captured plate itself; the independent native
    // behavior/runtime gate proves the shipping tree paints without it.
    const auto plate_node = std::find_if(
        ir.root.children.begin(), ir.root.children.end(), [](const auto& node) {
            const auto role = node.attributes.find("materialized_role");
            return role != node.attributes.end() &&
                role->second == "captured-paint-authority";
        });
    if (plate_node == ir.root.children.end()) {
        error = "materialized validation found no captured chrome authority";
        return false;
    }
    const auto plate_ref = plate_node->attributes.find("asset_ref");
    const auto plate_asset = plate_ref == plate_node->attributes.end()
        ? ir.asset_manifest.assets.end()
        : std::find_if(
            ir.asset_manifest.assets.begin(), ir.asset_manifest.assets.end(),
            [&](const auto& candidate) {
                return candidate.asset_id == plate_ref->second;
            });
    if (plate_asset == ir.asset_manifest.assets.end() ||
        !plate_asset->local_path || plate_asset->local_path->empty()) {
        error = "materialized chrome authority is not resolved";
        return false;
    }
    std::ifstream plate_input(*plate_asset->local_path, std::ios::binary);
    if (!plate_input) {
        error = "could not read materialized chrome authority";
        return false;
    }
    const std::vector<std::uint8_t> plate_bytes{
        std::istreambuf_iterator<char>(plate_input),
        std::istreambuf_iterator<char>()};
    auto destination = decode_png_rgba(plate_bytes.data(), plate_bytes.size());
    if (!destination.valid() || destination.width != native_render.width ||
        destination.height != native_render.height) {
        error = "materialized chrome authority extent does not match render";
        return false;
    }

    std::size_t composed = 0;
    const auto composite_ref = ir.root.attributes.find(
        "materialized_canvas_validation_asset");
    if (composite_ref != ir.root.attributes.end()) {
        const auto asset = std::find_if(
            ir.asset_manifest.assets.begin(), ir.asset_manifest.assets.end(),
            [&](const auto& candidate) {
                return candidate.asset_id == composite_ref->second;
            });
        if (asset == ir.asset_manifest.assets.end() || !asset->local_path ||
            asset->local_path->empty()) {
            error = "materialized canvas composite evidence is not resolved";
            return false;
        }
        std::ifstream input(*asset->local_path, std::ios::binary);
        if (!input) {
            error = "could not read materialized canvas composite evidence";
            return false;
        }
        const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const auto source = decode_png_rgba(bytes.data(), bytes.size());
        if (!source.valid() || source.width != destination.width ||
            source.height != destination.height) {
            error =
                "materialized canvas composite evidence extent does not match render";
            return false;
        }
        // Chromium has already resolved canvas-vs-DOM stacking in this sparse
        // evidence plane. Opaque pixels replace the canvas-free chrome and
        // transparent pixels preserve it; intermediate alpha would make the
        // result dependent on a second compositor and is therefore refused.
        for (std::size_t pixel = 0;
             pixel < destination.rgba.size(); pixel += 4) {
            const auto alpha = source.rgba[pixel + 3];
            if (alpha == 0) continue;
            if (alpha != 255) {
                error =
                    "materialized canvas composite evidence is not a sparse replacement plane";
                return false;
            }
            destination.rgba[pixel] = source.rgba[pixel];
            destination.rgba[pixel + 1] = source.rgba[pixel + 1];
            destination.rgba[pixel + 2] = source.rgba[pixel + 2];
            destination.rgba[pixel + 3] = 255;
        }
        ++composed;
    }
    for (const auto& node : ir.root.children) {
        if (composite_ref != ir.root.attributes.end()) break;
        const auto role = node.attributes.find("materialized_role");
        if (role == node.attributes.end() ||
            role->second != "executable-canvas-target")
            continue;
        const auto ref = node.attributes.find("asset_ref");
        if (ref == node.attributes.end()) {
            error = "materialized canvas target has no capture evidence";
            return false;
        }
        const auto asset = std::find_if(
            ir.asset_manifest.assets.begin(), ir.asset_manifest.assets.end(),
            [&](const auto& candidate) {
                return candidate.asset_id == ref->second;
            });
        if (asset == ir.asset_manifest.assets.end() ||
            !asset->local_path || asset->local_path->empty()) {
            error = "materialized canvas evidence is not resolved";
            return false;
        }
        std::ifstream input(*asset->local_path, std::ios::binary);
        if (!input) {
            error = "could not read materialized canvas evidence";
            return false;
        }
        const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const auto source = decode_png_rgba(bytes.data(), bytes.size());
        // Canvas snapshots are viewport-sized transparent evidence planes.
        // Refuse any other shape instead of guessing how a cropped bitmap maps
        // back into CSS/device coordinates.
        if (!source.valid() || source.width != destination.width ||
            source.height != destination.height) {
            error = "materialized canvas evidence extent does not match render";
            return false;
        }
        for (std::size_t pixel = 0;
             pixel < destination.rgba.size(); pixel += 4) {
            const float source_alpha = source.rgba[pixel + 3] / 255.0f;
            if (source_alpha <= 0.0f) continue;
            const float destination_alpha =
                destination.rgba[pixel + 3] / 255.0f;
            const float output_alpha = source_alpha +
                destination_alpha * (1.0f - source_alpha);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float source_value = source.rgba[pixel + channel];
                const float destination_value =
                    destination.rgba[pixel + channel];
                const float output = output_alpha > 0.0f
                    ? (source_value * source_alpha +
                       destination_value * destination_alpha *
                           (1.0f - source_alpha)) / output_alpha
                    : 0.0f;
                destination.rgba[pixel + channel] =
                    static_cast<std::uint8_t>(std::lround(output));
            }
            destination.rgba[pixel + 3] = static_cast<std::uint8_t>(
                std::lround(output_alpha * 255.0f));
        }
        ++composed;
    }
    if (composed == 0) {
        error = "materialized validation found no canvas evidence planes";
        return false;
    }
    rendered = encode_png_rgba(destination);
    if (rendered.empty()) {
        error = "could not encode materialized validation composition";
        return false;
    }
    return true;
}

}  // namespace

ReferenceRegistration resolve_reference_registration(
    const pulp::view::DesignIR& ir,
    const BrowserCaptureValidationOptions& options,
    int reference_width,
    int reference_height,
    int rendered_width,
    int rendered_height) {
    ReferenceRegistration registration;
    if (reference_width <= 0 || reference_height <= 0 ||
        rendered_width <= 0 || rendered_height <= 0) {
        registration.reason =
            "never scored: an image has no usable dimensions (reference " +
            extent(reference_width, reference_height) + ", render " +
            extent(rendered_width, rendered_height) + ")";
        return registration;
    }

    // An explicit rect from the caller wins. It is a deliberate override, not a
    // guess this function is entitled to second-guess.
    if (options.reference_crop_width > 0 &&
        options.reference_crop_height > 0) {
        registration.registered = true;
        registration.x = options.reference_crop_x;
        registration.y = options.reference_crop_y;
        registration.width = options.reference_crop_width;
        registration.height = options.reference_crop_height;
        return registration;
    }

    // A materialized composition renders the complete accepted Chromium
    // surface: its authored-frame attributes describe the executable behavior
    // tree inside that surface, not a crop of the rendered visual authority.
    // When the reference and render already have identical extents, comparing
    // the whole images is therefore the only coherent registration. Applying
    // the behavior-frame crop here mixes the presentation and authored
    // coordinate spaces and rejects the exact full-surface render.
    const auto materialized_authority = ir.root.attributes.find(
        "materialized_visual_authority");
    if (materialized_authority != ir.root.attributes.end() &&
        materialized_authority->second ==
            "browser:chrome+native-canvases" &&
        reference_width == rendered_width &&
        reference_height == rendered_height) {
        registration.registered = true;
        registration.width = reference_width;
        registration.height = reference_height;
        return registration;
    }

    const auto frame_x =
        attribute_number(ir.root, "browser_authored_frame_x");
    const auto frame_y =
        attribute_number(ir.root, "browser_authored_frame_y");
    const auto frame_width =
        attribute_number(ir.root, "browser_authored_frame_width");
    const auto frame_height =
        attribute_number(ir.root, "browser_authored_frame_height");
    if (frame_x && frame_y && frame_width && frame_height &&
        *frame_width > 0.0 && *frame_height > 0.0) {
        // The frame is recorded in CSS px and the reference is raster at the
        // capture's device scale, so the rect is scaled here rather than at the
        // emitter: one device scale in the envelope, no second copy to drift.
        const double dpr =
            attribute_number(ir.root, "browser_device_scale_factor")
                .value_or(1.0);
        const double scale = dpr > 0.0 ? dpr : 1.0;
        registration.registered = true;
        registration.x = static_cast<int>(std::lround(*frame_x * scale));
        registration.y = static_cast<int>(std::lround(*frame_y * scale));
        // The render's extent is the root's size ROUNDED, and the frame is the
        // same geometry scaled, so a fractional CSS height can land the two a
        // pixel apart. Snap a one-pixel disagreement to the render: the
        // comparison needs identical extents, and a pixel of rounding is not a
        // misregistration. A LARGER disagreement is one -- the frame and the
        // root would be describing different boxes -- and is left to be refused
        // by the extent check after the crop.
        const auto snap_to_render = [](int value, int rendered) {
            return std::abs(value - rendered) <= 1 ? rendered : value;
        };
        registration.width = snap_to_render(
            static_cast<int>(std::lround(*frame_width * scale)),
            rendered_width);
        registration.height = snap_to_render(
            static_cast<int>(std::lround(*frame_height * scale)),
            rendered_height);
        return registration;
    }

    // Nothing to crop TO because nothing was cropped FROM: the render already
    // covers the reference whole, so the identity rect registers them.
    if (reference_width == rendered_width &&
        reference_height == rendered_height) {
        registration.registered = true;
        registration.width = reference_width;
        registration.height = reference_height;
        return registration;
    }

    registration.reason =
        "never scored: the capture records no authored frame and the "
        "reference (" + extent(reference_width, reference_height) +
        ") is not the render (" + extent(rendered_width, rendered_height) +
        "); a size mismatch is compared over the wrong pixels";
    return registration;
}

BrowserCaptureValidationResult validate_browser_capture_design_ir(
    const pulp::view::DesignIR& ir,
    const BrowserCaptureValidationOptions& options) {
    BrowserCaptureValidationResult result;
    if (options.width <= 0 || options.height <= 0 ||
        options.reference.empty() || options.rendered.empty()) {
        result.error = "browser capture validation options are incomplete";
        return result;
    }

    // Native lowering deliberately leaves unsupported painted elements as
    // explicit, unpainted fallbacks. A dark empty render can otherwise score
    // extremely well against a dark UI: Spectr's two full-window canvas holes
    // reported 88% on the main editor and 97% with Settings open. Refuse to
    // produce a parity score until every such hole has a real native painter.
    // The count is the authority; area is diagnostic and may exceed 1 when
    // overlapping fallbacks each cover the panel.
    const auto fallback_key =
        ir.root.attributes.find("native_nodes_element_capture_fallback");
    if (fallback_key != ir.root.attributes.end()) {
        const auto fallback_count = attribute_uint64(
            ir.root, "native_nodes_element_capture_fallback");
        if (!fallback_count) {
            result.error =
                "native panel validation found malformed fallback coverage";
            return result;
        }
        if (*fallback_count > 0) {
            result.error =
                "native panel validation refused: " +
                std::to_string(*fallback_count) +
                " painted element fallback(s) have no native painter";
            if (const auto fraction = attribute_number(
                    ir.root, "native_nodes_unpainted_area_fraction")) {
                result.error += " (unpainted area fraction " +
                                std::to_string(*fraction) + ")";
            }
            return result;
        }
    }

    // `asset_ref` is the portable IR contract; native ImageView materialization
    // consumes the resolved `asset_path`. Browser validation must exercise the
    // same manifest-resolution step as generated native exports, otherwise a
    // perfectly captured canvas arrives as an empty ImageView and the parity
    // gate scores a black frame instead of the artifact it claims to validate.
    auto render_ir = ir;
    pulp::view::enrich_imported_image_asset_metadata(
        render_ir, render_ir.asset_manifest);
    auto root = pulp::view::build_native_view_tree(
        render_ir, render_ir.asset_manifest);
    if (!root) {
        result.error = "could not materialize browser capture DesignIR";
        return result;
    }
    auto rendered = pulp::view::render_to_png(
        *root, static_cast<std::uint32_t>(options.width),
        static_cast<std::uint32_t>(options.height), 2.0f, options.backend);
    if (rendered.empty()) {
        result.error = "DesignIR/Skia render failed";
        return result;
    }
    // Static import validation needs the accepted initial canvas pixels in
    // order to compare the complete Chromium frame. This is evidence-only:
    // the published IR remains executable CanvasWidget targets, and the live
    // native runtime has a separate no-reference-plane gate proving it paints
    // those streams itself.
    if (!compose_materialized_canvas_evidence(ir, rendered, result.error))
        return result;
    if (!write_bytes_atomically(options.rendered, rendered, result.error))
        return result;

    // Read the reference once and REGISTER it against the render before any
    // pixel is compared. The capture is deliberately larger than the design --
    // the panel's root carries its own padding and the harness grows the extent
    // so drop shadows and absolutely positioned decoration are not clipped --
    // so the two images hold the same panel at different origins. Comparing
    // them unregistered reads the same pixel box out of two different pictures
    // and returns a number for it, which is how a panel that differs by 5.6% of
    // its pixels was reported at 31% similar.
    //
    // Registering HERE, where the comparison is computed, rather than at the
    // line that prints it, is the whole point: the printout is one consumer of
    // this number and every other one -- the --fail-below gate, the debug
    // envelope, any future caller -- would otherwise keep scoring the wrong
    // pixels.
    std::vector<std::uint8_t> reference_bytes;
    {
        std::ifstream input(options.reference, std::ios::binary);
        if (!input) {
            result.error = "could not read browser validation reference";
            return result;
        }
        reference_bytes.assign(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>());
    }

    const auto reference_extent =
        pulp::view::inspect_png_metadata(reference_bytes);
    const auto rendered_extent = pulp::view::inspect_png_metadata(rendered);
    const auto registration = resolve_reference_registration(
        ir, options,
        static_cast<int>(reference_extent.width),
        static_cast<int>(reference_extent.height),
        static_cast<int>(rendered_extent.width),
        static_cast<int>(rendered_extent.height));

    std::string refusal = registration.reason;
    const bool crops_the_reference =
        registration.registered &&
        (registration.x != 0 || registration.y != 0 ||
         registration.width != static_cast<int>(reference_extent.width) ||
         registration.height != static_cast<int>(reference_extent.height));
    if (crops_the_reference) {
        auto cropped = pulp::view::crop_png(
            reference_bytes, static_cast<std::uint32_t>(registration.x),
            static_cast<std::uint32_t>(registration.y),
            static_cast<std::uint32_t>(registration.width),
            static_cast<std::uint32_t>(registration.height));
        if (cropped.empty()) {
            refusal =
                "never scored: the registration rect does not lie inside the "
                "reference";
        } else {
            reference_bytes = std::move(cropped);
        }
    }
    if (refusal.empty()) {
        // Equal extents are the invariant that makes the reported number
        // honest. compare_screenshots scores the OVERLAP and then scales the
        // result by how much of the images that overlap covers, so unequal
        // extents produce a percentage that contradicts the differing-pixel
        // count printed beside it -- a 200x120 page captured on a 1280x800
        // viewport reported "2% (114/96000 pixels differ)", which is 99.88%
        // identical scaled by 0.0234. Refusing here is what keeps the ratio at
        // 1, so the printed percentage is exactly 1 - differing/total.
        //
        // It also catches a clamped crop: crop_png CLAMPS a rect that overruns
        // its image, returning a SMALLER picture rather than an error, which is
        // the same misregistration wearing a correct-looking rect.
        const auto registered_extent =
            pulp::view::inspect_png_metadata(reference_bytes);
        if (registered_extent.width != rendered_extent.width ||
            registered_extent.height != rendered_extent.height) {
            refusal =
                "never scored: the registered reference (" +
                extent(static_cast<int>(registered_extent.width),
                       static_cast<int>(registered_extent.height)) +
                ") is not the size of the render (" +
                extent(static_cast<int>(rendered_extent.width),
                       static_cast<int>(rendered_extent.height)) + ")";
        }
    }

    // The diff is written either way. When registration failed it is the
    // evidence for WHY, and a refusal with no picture to look at is a dead end.
    if (!options.diff.empty()) {
        const std::vector<std::uint8_t>& reference = reference_bytes;
        const auto diff = pulp::view::generate_diff_image(reference, rendered);
        if (diff.empty()) {
            result.error = "could not generate browser validation diff";
            return result;
        }
        if (!write_bytes_atomically(options.diff, diff, result.error))
            return result;
    }

    if (!refusal.empty()) {
        // The validation RAN and its artifacts exist; only the oracle is
        // unusable. Reporting this as an error would discard a good import over
        // a comparison it does not depend on.
        result.valid = true;
        result.scored = false;
        result.registration_reason = std::move(refusal);
        return result;
    }

    const auto comparison =
        pulp::view::compare_screenshots(reference_bytes, rendered);
    if (!comparison.valid) {
        result.error = comparison.error;
        return result;
    }
    result.similarity = comparison.similarity;
    result.diff_pixels = comparison.diff_pixels;
    result.total_pixels = comparison.total_pixels;
    result.mean_error = comparison.mean_error;
    const float gate = options.fail_below_percent >= 0.0f
        ? options.fail_below_percent / 100.0f
        : pulp::view::kDefaultSimilarityThreshold;
    result.passes = comparison.passes(gate);
    result.scored = true;
    result.valid = true;
    return result;
}

}  // namespace pulp::import_design
