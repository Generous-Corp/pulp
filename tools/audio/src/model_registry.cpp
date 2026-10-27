#include <pulp/tools/audio/model_registry.hpp>

namespace pulp::tools::audio {

// The audio model catalog. The registry type + URL resolution + lookup are generic
// (pulp::runtime); only this data lives here.
const std::vector<RegisteredModel>& registered_models() {
    static const std::vector<RegisteredModel> models = {{
        .model_id = "clap_music_audioset_v1",
        .display_name = "CLAP Music AudioSet",
        .backend = "clap",
        .checkpoint_ref = "hf://lukewys/laion_clap/music.pt",
        .task_tags = {"music", "excerpt_find"},
        .size_bytes = 2523293286ULL,
        .download_url = "https://huggingface.co/lukewys/laion_clap/resolve/main/music.pt",
        // No digest is recorded yet, so install_model() refuses this entry until the
        // caller passes allow_unverified (a CLI/UI exposes that as --unverified).
        // Record the digest of a verified download here to lift that requirement.
        .sha256 = "",
        .auto_downloadable = true,
    }};
    return models;
}

}  // namespace pulp::tools::audio
