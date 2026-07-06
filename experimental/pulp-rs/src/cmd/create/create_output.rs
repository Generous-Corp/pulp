use std::path::{Component, Path, PathBuf};

use crate::error::{CliError, Result};

use super::{to_lower_name, CreateArgs};

fn lexical_normalize(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                out.pop();
            }
            _ => out.push(component.as_os_str()),
        }
    }
    out
}

fn path_is_within(path: &Path, root: &Path) -> bool {
    lexical_normalize(path).starts_with(lexical_normalize(root))
}

fn user_home_dir() -> Option<PathBuf> {
    let key = if cfg!(windows) { "USERPROFILE" } else { "HOME" };
    std::env::var_os(key)
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}

fn configured_path_value(value: &str) -> Option<String> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return None;
    }
    if trimmed.len() >= 2
        && ((trimmed.starts_with('"') && trimmed.ends_with('"'))
            || (trimmed.starts_with('\'') && trimmed.ends_with('\'')))
    {
        return Some(trimmed[1..trimmed.len() - 1].to_owned());
    }
    Some(trimmed.to_owned())
}

fn expand_configured_path(value: &str, cwd: &Path, user_home: Option<&Path>) -> Option<PathBuf> {
    let configured = configured_path_value(value)?;
    let mut path = PathBuf::from(&configured);
    if configured.starts_with('~') {
        if let Some(home) = user_home {
            path = if configured == "~" {
                home.to_path_buf()
            } else {
                home.join(configured.get(2..).unwrap_or_default())
            };
        }
    }
    if path.is_absolute() {
        Some(path)
    } else {
        Some(cwd.join(path))
    }
}

fn configured_projects_base_dir(cwd: &Path) -> Result<Option<PathBuf>> {
    let home = user_home_dir();
    if let Some(value) = std::env::var_os("PULP_PROJECTS_DIR") {
        if let Some(base) = expand_configured_path(&value.to_string_lossy(), cwd, home.as_deref()) {
            return Ok(Some(base));
        }
    }

    let Some(path) = crate::config::config_path() else {
        return Ok(None);
    };
    let Ok(doc) = crate::config::read(&path) else {
        return Ok(None);
    };
    let configured = crate::config::read_value(&doc, "create", "projects_dir");
    Ok(expand_configured_path(&configured, cwd, home.as_deref()))
}

/// Resolve an output directory given the parsed args + project root
/// (when in-tree).
///
/// # Errors
///
/// [`CliError::BadUsage`] when in-tree mode is used outside a checkout
/// or when the output path collides with an existing directory or violates
/// create's standalone/in-tree containment policy.
pub(super) fn resolve_out_dir(
    args: &CreateArgs,
    root: Option<&Path>,
    cwd: &Path,
) -> Result<PathBuf> {
    let lower = to_lower_name(&args.name);
    let out_dir = if let Some(o) = args.output.as_deref() {
        if o.is_absolute() {
            o.to_path_buf()
        } else {
            cwd.join(o)
        }
    } else if args.in_tree {
        let Some(root) = root else {
            return Err(CliError::BadUsage(
                "Error: --in-tree/--example can only be used from inside the Pulp repo".to_owned(),
            ));
        };
        root.join("examples").join(&lower)
    } else {
        let base = configured_projects_base_dir(cwd)?.unwrap_or_else(|| {
            // Standalone default: alongside the Pulp repo root when known,
            // otherwise cwd. Mirrors C++ `resolve_create_projects_base_dir`
            // after env/config overrides are absent.
            root.and_then(Path::parent)
                .map_or_else(|| cwd.to_path_buf(), Path::to_path_buf)
        });
        base.join(&lower)
    };

    if args.in_tree {
        let Some(root) = root else {
            return Err(CliError::BadUsage(
                "Error: --in-tree/--example can only be used from inside the Pulp repo".to_owned(),
            ));
        };
        let examples_root = root.join("examples");
        if !path_is_within(&out_dir, &examples_root) {
            return Err(CliError::BadUsage(format!(
                "Error: --in-tree projects must live under {}",
                examples_root.display()
            )));
        }
    } else if let Some(root) = root {
        if path_is_within(&out_dir, root) {
            return Err(CliError::BadUsage(format!(
                "Error: standalone product projects must live outside the Pulp repo\n  Use --in-tree to scaffold under examples/, or choose --output outside\n  {}",
                root.display()
            )));
        }
    }

    if out_dir.exists() {
        return Err(CliError::BadUsage(format!(
            "Error: {} already exists",
            out_dir.display()
        )));
    }
    Ok(out_dir)
}

#[cfg(test)]
mod tests {
    use std::fs;

    use super::*;

    #[test]
    fn resolve_out_dir_rejects_in_tree_without_root() {
        let td = tempfile::tempdir().unwrap();
        let args = CreateArgs {
            name: "demo".to_owned(),
            in_tree: true,
            ci_mode: true,
            ..CreateArgs::default()
        };
        let err = resolve_out_dir(&args, None, td.path()).unwrap_err();
        assert!(err.to_string().contains("--in-tree"));
    }

    #[test]
    fn resolve_out_dir_rejects_existing_dir() {
        let td = tempfile::tempdir().unwrap();
        let existing = td.path().join("parent").join("demo");
        fs::create_dir_all(&existing).unwrap();
        let args = CreateArgs {
            name: "demo".to_owned(),
            output: Some(existing),
            ci_mode: true,
            ..CreateArgs::default()
        };
        let err = resolve_out_dir(&args, None, td.path()).unwrap_err();
        assert!(err.to_string().contains("already exists"));
    }

    #[test]
    fn resolve_out_dir_rejects_standalone_inside_checkout() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path().join("pulp");
        fs::create_dir_all(&root).unwrap();
        let args = CreateArgs {
            name: "Inside".to_owned(),
            output: Some(root.join("build/generated")),
            ci_mode: true,
            ..CreateArgs::default()
        };
        let err = resolve_out_dir(&args, Some(&root), td.path()).unwrap_err();
        assert!(err.to_string().contains("standalone product projects"));
    }

    #[test]
    fn resolve_out_dir_rejects_in_tree_output_outside_examples() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path().join("pulp");
        fs::create_dir_all(root.join("examples")).unwrap();
        let args = CreateArgs {
            name: "Outside".to_owned(),
            output: Some(td.path().join("outside")),
            in_tree: true,
            ci_mode: true,
            ..CreateArgs::default()
        };
        let err = resolve_out_dir(&args, Some(&root), td.path()).unwrap_err();
        assert!(err
            .to_string()
            .contains("--in-tree projects must live under"));
    }

    #[test]
    fn resolve_out_dir_uses_pulp_projects_dir_env() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path().join("pulp");
        let env_base = td.path().join("configured-output");
        let pulp_home = td.path().join("home");
        let env_value = format!("  \"{}\"  ", env_base.display());
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_PROJECTS_DIR", Some(&env_value)),
            ("PULP_HOME", Some(pulp_home.to_str().unwrap())),
        ]);
        let args = CreateArgs {
            name: "Demo".to_owned(),
            ci_mode: true,
            ..CreateArgs::default()
        };

        let resolved = resolve_out_dir(&args, Some(&root), td.path()).unwrap();
        assert_eq!(resolved, env_base.join("demo"));
    }

    #[test]
    fn resolve_out_dir_env_precedes_create_projects_config() {
        let td = tempfile::tempdir().unwrap();
        let pulp_home = td.path().join("pulp-home");
        fs::create_dir_all(&pulp_home).unwrap();
        fs::write(
            pulp_home.join("config.toml"),
            "[create]\nprojects_dir = \"config-output\"\n",
        )
        .unwrap();
        let env_base = td.path().join("env-output");
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_PROJECTS_DIR", Some(env_base.to_str().unwrap())),
            ("PULP_HOME", Some(pulp_home.to_str().unwrap())),
        ]);
        let args = CreateArgs {
            name: "Demo".to_owned(),
            ci_mode: true,
            ..CreateArgs::default()
        };

        let resolved = resolve_out_dir(&args, Some(&td.path().join("pulp")), td.path()).unwrap();
        assert_eq!(resolved, env_base.join("demo"));
    }

    #[test]
    fn resolve_out_dir_uses_create_projects_dir_config() {
        let td = tempfile::tempdir().unwrap();
        let cwd = td.path().join("cwd");
        let pulp_home = td.path().join("pulp-home");
        fs::create_dir_all(&pulp_home).unwrap();
        fs::write(
            pulp_home.join("config.toml"),
            "[create]\nprojects_dir = \"configured-projects\"\n",
        )
        .unwrap();
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_PROJECTS_DIR", None),
            ("PULP_HOME", Some(pulp_home.to_str().unwrap())),
        ]);
        let args = CreateArgs {
            name: "Demo".to_owned(),
            ci_mode: true,
            ..CreateArgs::default()
        };

        let resolved = resolve_out_dir(&args, Some(&td.path().join("pulp")), &cwd).unwrap();
        assert_eq!(resolved, cwd.join("configured-projects").join("demo"));
    }

    #[test]
    fn resolve_out_dir_ignores_unreadable_create_projects_config() {
        let td = tempfile::tempdir().unwrap();
        let root = td.path().join("pulp");
        let pulp_home = td.path().join("pulp-home");
        fs::create_dir_all(&root).unwrap();
        fs::create_dir_all(&pulp_home).unwrap();
        fs::write(pulp_home.join("config.toml"), "[create\nprojects_dir =").unwrap();
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_PROJECTS_DIR", None),
            ("PULP_HOME", Some(pulp_home.to_str().unwrap())),
        ]);
        let args = CreateArgs {
            name: "Demo".to_owned(),
            ci_mode: true,
            ..CreateArgs::default()
        };

        let resolved = resolve_out_dir(&args, Some(&root), td.path()).unwrap();
        assert_eq!(resolved, td.path().join("demo"));
    }

    #[test]
    fn resolve_out_dir_expands_tilde_in_configured_base() {
        let td = tempfile::tempdir().unwrap();
        let pulp_home = td.path().join("pulp-home");
        let user_home = td.path().join("user-home");
        fs::create_dir_all(&pulp_home).unwrap();
        fs::write(
            pulp_home.join("config.toml"),
            "[create]\nprojects_dir = \"~/PulpProjects\"\n",
        )
        .unwrap();
        let _env = crate::test_support::EnvVarGuard::set_many(&[
            ("PULP_PROJECTS_DIR", None),
            ("PULP_HOME", Some(pulp_home.to_str().unwrap())),
            ("HOME", Some(user_home.to_str().unwrap())),
            ("USERPROFILE", Some(user_home.to_str().unwrap())),
        ]);
        let args = CreateArgs {
            name: "Demo".to_owned(),
            ci_mode: true,
            ..CreateArgs::default()
        };

        let resolved = resolve_out_dir(&args, Some(&td.path().join("pulp")), td.path()).unwrap();
        assert_eq!(resolved, user_home.join("PulpProjects").join("demo"));
    }

    #[test]
    fn resolve_out_dir_uses_explicit_output_when_absolute() {
        let td = tempfile::tempdir().unwrap();
        let abs = td.path().join("nested/out");
        let env_output = td.path().join("env-output");
        let _env = crate::test_support::EnvVarGuard::set_many(&[(
            "PULP_PROJECTS_DIR",
            Some(env_output.to_str().unwrap()),
        )]);
        let args = CreateArgs {
            name: "Foo".to_owned(),
            output: Some(abs.clone()),
            ci_mode: true,
            ..CreateArgs::default()
        };
        // Absolute --output wins regardless of root/cwd.
        let cwd = std::env::temp_dir();
        let resolved = resolve_out_dir(&args, None, &cwd).unwrap();
        assert_eq!(resolved, abs);
    }

    #[test]
    fn resolve_out_dir_resolves_relative_output_against_cwd() {
        let td = tempfile::tempdir().unwrap();
        let args = CreateArgs {
            name: "Foo".to_owned(),
            output: Some(PathBuf::from("relout")),
            ci_mode: true,
            ..CreateArgs::default()
        };
        let resolved = resolve_out_dir(&args, None, td.path()).unwrap();
        assert_eq!(resolved, td.path().join("relout"));
    }
}
