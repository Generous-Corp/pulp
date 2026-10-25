---
breaking = true
summary = "Model downloads without an expected sha256 are refused unless the caller opts in"
---

## Breaking changes
- `pulp::runtime::download_file` refuses a `DownloadRequest` whose `expected_sha256` is empty. The result carries `ok == false`, `unverified == true`, and an error naming the URL; nothing is fetched and no `.part` file or destination directory is created. Set `DownloadRequest::allow_unverified = true` to accept the bytes anyway; the result then still reports `unverified == true` and `sha256` holds the digest the transfer produced.
- `pulp::runtime::install_model` gained a trailing `allow_unverified` parameter (default `false`). A `ModelEntry` or bundled `ModelAsset` with an empty `sha256` is now refused with an error containing `unverified` instead of installing without a check.
- The shipped audio model registry entry `clap_music_audioset_v1` (`tools/audio/src/model_registry.cpp`, `https://huggingface.co/lukewys/laion_clap/resolve/main/music.pt`) still carries an empty digest, so installing it requires the opt-in until a verified digest is recorded there.
- A redirect `Location` that moves a transfer from `https` to `http` is refused; the download fails with an error containing `downgrade`. `http` to `https` and same-scheme redirects are unaffected.

## Migration steps
- Record the sha256 of a verified download on each registry entry you ship; that lifts the opt-in requirement for it.
- Code that must fetch a file nobody has a digest for sets `allow_unverified` on the request (or passes it to `install_model`) and surfaces that choice to the user, for example as a `--unverified` flag.
- Treat `DownloadResult::unverified` as a signal to display or log, not as a success state: with `ok == true` it means the caller accepted bytes no digest vouches for.
