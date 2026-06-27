use crate::color;
use crate::tool_registry::{LocateResult, ToolDescriptor};

pub(super) fn tool_available_on_platform(tool: &ToolDescriptor, platform: &str) -> bool {
    tool.binary_sources.contains_key(platform)
        || tool.install_method == "python_pip"
        || tool.install_method == "npm_package"
}

pub(super) fn status_label(tool: &ToolDescriptor, loc: &LocateResult, platform: &str) -> String {
    if loc.found && loc.source == "pulp-managed" {
        format!("{}installed{}", color::green(), color::reset())
    } else if loc.found {
        format!(
            "{}system ({}){}",
            color::yellow(),
            loc.path.display(),
            color::reset()
        )
    } else if tool_available_on_platform(tool, platform) {
        format!("{}available{}", color::dim(), color::reset())
    } else {
        format!(
            "{}not available for {platform}{}",
            color::red(),
            color::reset()
        )
    }
}
