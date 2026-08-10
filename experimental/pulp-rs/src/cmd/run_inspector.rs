//! Rejection contract for retired Development Inspector `pulp run` flags.

pub(crate) fn parse_arg(
    args: &[String],
    index: usize,
) -> Result<Option<usize>, String> {
    let argument = &args[index];
    if argument == "--inspect"
        || argument.starts_with("--inspect=")
        || argument == "--inspect-capability"
        || argument.starts_with("--inspect-capability=")
        || argument == "--inspect-runtime-eval"
    {
        return Err(
            "pulp run inspector flags are retired; use `pulp control` with an exact instance and typed operation"
                .to_owned(),
        );
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::super::run_parse::parse_run_options;

    fn argv(items: &[&str]) -> Vec<String> {
        items.iter().map(|item| (*item).to_owned()).collect()
    }

    #[test]
    fn rejects_retired_inspector_flags_with_control_guidance() {
        for retired in [
            argv(&["--inspect"]),
            argv(&["--inspect=observe"]),
            argv(&["--inspect-capability", "session.describe"]),
            argv(&["--inspect-capability=state.write"]),
            argv(&["--inspect-runtime-eval"]),
        ] {
            let result = parse_run_options(&retired);
            assert!(result.error.contains("retired"));
            assert!(result.error.contains("pulp control"));
        }
    }
}
