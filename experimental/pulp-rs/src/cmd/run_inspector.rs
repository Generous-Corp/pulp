//! Development Inspector options shared by the Rust `pulp run` parser and
//! launcher-environment assembly.

use super::run_parse::RunOptions;

pub(crate) fn parse_arg(
    args: &[String],
    index: usize,
    options: &mut RunOptions,
) -> Result<Option<usize>, String> {
    let argument = &args[index];
    if argument == "--inspect" {
        options.inspector_profile = "develop".to_owned();
        return Ok(Some(index + 1));
    }
    if let Some(profile) = argument.strip_prefix("--inspect=") {
        if !matches!(profile, "off" | "observe" | "develop" | "custom") {
            return Err("--inspect profile must be off, observe, develop, or custom".to_owned());
        }
        options.inspector_profile = profile.to_owned();
        return Ok(Some(index + 1));
    }
    if argument == "--inspect-capability" {
        let Some(capability) = args.get(index + 1) else {
            return Err("--inspect-capability requires a capability id".to_owned());
        };
        if capability.is_empty() || capability.starts_with('-') {
            return Err("--inspect-capability requires a capability id".to_owned());
        }
        if capability.contains(',') {
            return Err("--inspect-capability must name exactly one capability".to_owned());
        }
        options.inspector_capabilities.push(capability.clone());
        return Ok(Some(index + 2));
    }
    if let Some(capability) = argument.strip_prefix("--inspect-capability=") {
        if capability.is_empty() {
            return Err("--inspect-capability= requires a capability id".to_owned());
        }
        if capability.contains(',') {
            return Err("--inspect-capability must name exactly one capability".to_owned());
        }
        options.inspector_capabilities.push(capability.to_owned());
        return Ok(Some(index + 1));
    }
    if argument == "--inspect-runtime-eval" {
        options.inspector_runtime_eval = true;
        return Ok(Some(index + 1));
    }
    Ok(None)
}

pub(crate) fn validate(options: &RunOptions) -> Result<(), String> {
    if !options.inspector_capabilities.is_empty() && options.inspector_profile != "custom" {
        return Err("--inspect-capability requires --inspect=custom".to_owned());
    }
    if options.inspector_profile == "custom" && options.inspector_capabilities.is_empty() {
        return Err("--inspect=custom requires at least one --inspect-capability".to_owned());
    }
    let has = |capability: &str| {
        options
            .inspector_capabilities
            .iter()
            .any(|item| item == capability)
    };
    if options.inspector_profile == "custom"
        && (has("state.write") || has("authoring.tweaks") || has("runtime.eval"))
        && !has("session.control")
    {
        return Err("custom inspector mutation capabilities require session.control".to_owned());
    }
    if options.inspector_profile == "custom"
        && has("runtime.eval")
        && !options.inspector_runtime_eval
    {
        return Err(
            "runtime.eval requires the separate --inspect-runtime-eval acknowledgement".to_owned(),
        );
    }
    if options.inspector_runtime_eval
        && options.inspector_profile != "develop"
        && !(options.inspector_profile == "custom" && has("runtime.eval"))
    {
        return Err(
            "--inspect-runtime-eval requires --inspect=develop or a custom runtime.eval capability"
                .to_owned(),
        );
    }
    Ok(())
}

pub(crate) fn append_launch_env(options: &RunOptions, environment: &mut Vec<(String, String)>) {
    if !options.inspector_profile.is_empty() {
        environment.push((
            "PULP_INSPECT_PROFILE".to_owned(),
            options.inspector_profile.clone(),
        ));
    }
    if !options.inspector_capabilities.is_empty() {
        environment.push((
            "PULP_INSPECT_CAPABILITIES".to_owned(),
            options.inspector_capabilities.join(","),
        ));
    }
    if options.inspector_runtime_eval {
        environment.push(("PULP_INSPECT_RUNTIME_EVAL".to_owned(), "1".to_owned()));
    }
}

#[cfg(test)]
mod tests {
    use super::super::run_parse::{assemble_launch_args, assemble_launch_env, parse_run_options};

    fn argv(items: &[&str]) -> Vec<String> {
        items.iter().map(|item| (*item).to_owned()).collect()
    }

    #[test]
    fn parses_profiles_and_custom_capabilities() {
        let develop = parse_run_options(&argv(&["--inspect"]));
        assert!(develop.error.is_empty());
        assert_eq!(develop.inspector_profile, "develop");

        let custom = parse_run_options(&argv(&[
            "--inspect=custom",
            "--inspect-capability",
            "session.describe",
            "--inspect-capability",
            "session.control",
            "--inspect-capability=state.write",
        ]));
        assert!(custom.error.is_empty());
        assert_eq!(
            custom.inspector_capabilities,
            vec!["session.describe", "session.control", "state.write"]
        );
        assert_eq!(
            assemble_launch_env(&custom),
            vec![
                ("PULP_INSPECT_PROFILE".to_owned(), "custom".to_owned()),
                (
                    "PULP_INSPECT_CAPABILITIES".to_owned(),
                    "session.describe,session.control,state.write".to_owned(),
                ),
            ]
        );

        for invalid in [
            argv(&["--inspect=bad"]),
            argv(&["--inspect=custom"]),
            argv(&["--inspect=develop", "--inspect-capability=session.describe"]),
            argv(&["--inspect=custom", "--inspect-capability=state.write"]),
            argv(&[
                "--inspect=custom",
                "--inspect-capability=state.write,session.control",
            ]),
            argv(&[
                "--inspect=custom",
                "--inspect-capability",
                "state.write,session.control",
            ]),
        ] {
            assert!(!parse_run_options(&invalid).error.is_empty());
        }
    }

    #[test]
    fn runtime_evaluation_requires_the_separate_acknowledgement() {
        let develop = parse_run_options(&argv(&["--inspect=develop", "--inspect-runtime-eval"]));
        assert!(develop.error.is_empty());
        assert!(develop.inspector_runtime_eval);
        assert_eq!(
            assemble_launch_args(&develop),
            vec!["--inspect-runtime-eval".to_owned()]
        );
        assert_eq!(
            assemble_launch_env(&develop),
            vec![
                ("PULP_INSPECT_PROFILE".to_owned(), "develop".to_owned()),
                ("PULP_INSPECT_RUNTIME_EVAL".to_owned(), "1".to_owned()),
            ]
        );

        let custom = parse_run_options(&argv(&[
            "--inspect=custom",
            "--inspect-capability=session.control",
            "--inspect-capability=runtime.eval",
            "--inspect-runtime-eval",
        ]));
        assert!(custom.error.is_empty());

        for invalid in [
            argv(&["--inspect-runtime-eval"]),
            argv(&["--inspect=observe", "--inspect-runtime-eval"]),
            argv(&[
                "--inspect=custom",
                "--inspect-capability=session.control",
                "--inspect-capability=runtime.eval",
            ]),
            argv(&[
                "--inspect=custom",
                "--inspect-capability=runtime.eval",
                "--inspect-runtime-eval",
            ]),
        ] {
            assert!(!parse_run_options(&invalid).error.is_empty());
        }
    }
}
