//! Cross-test helpers for parity fixtures.
//!
//! Each integration test binary gets its own copy of this module
//! because `cargo` compiles them independently. That's fine — the
//! alternative (a shared crate) would leak into the production
//! build surface, and the helpers are small.
//!
//! ## What's covered
//!
//! - [`fixture_dir`] — path lookup under `tests/fixtures/<category>/<name>`.
//! - [`normalise_path_field`] — portability rewrite for abs paths
//!   that include the fixture directory in them.
//! - [`run_pulp_rs`] — standard subprocess invocation with a fresh
//!   `PULP_HOME`.
//!
//! Individual parity tests extend these for command-specific
//! normalisations (e.g. `config_path` → `<CONFIG_PATH>`).

#![allow(dead_code)] // each test binary uses a subset of helpers

use std::path::PathBuf;

use serde_json::Value;

/// Resolve `tests/fixtures/<category>/<name>`.
pub fn fixture_dir(category: &str, name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join(category)
        .join(name)
}

/// Rewrite a JSON string field that holds an absolute path so the
/// machine-specific prefix is replaced by a `<FIXTURE:...>` token.
/// The fixture directory is `tests/fixtures/<category>/<name>`. Both
/// forward and backward slashes are matched so the same normalised
/// output falls out on every platform.
pub fn normalise_path_field(v: &mut Value, key: &str, category: &str, name: &str) {
    let target = format!("fixtures/{category}/{name}");
    let target_back = format!("fixtures\\{category}\\{name}");
    let Some(obj) = v.as_object_mut() else {
        return;
    };
    let Some(item) = obj.get_mut(key) else {
        return;
    };
    let Some(s) = item.as_str() else {
        return;
    };
    if s.is_empty() {
        return;
    }
    let normalised = s.replace('\\', "/");
    if let Some(idx) = normalised.find(&target) {
        let tail = &normalised[idx + target.len()..];
        *item = Value::String(format!("<FIXTURE:{name}>{tail}"));
    } else if let Some(idx) = s.find(&target_back) {
        let tail = &s[idx + target_back.len()..].replace('\\', "/");
        *item = Value::String(format!("<FIXTURE:{name}>{tail}"));
    }
}

/// Replace a JSON string field with a literal token regardless of
/// content. Used for fields like `config_path` where the absolute
/// path lives in a per-run tempdir that the test controls.
pub fn replace_field(v: &mut Value, key: &str, replacement: &str) {
    if let Some(obj) = v.as_object_mut() {
        if obj.contains_key(key) {
            obj.insert(key.to_owned(), Value::String(replacement.to_owned()));
        }
    }
}
