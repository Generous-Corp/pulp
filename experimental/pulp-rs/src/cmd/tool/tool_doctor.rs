use std::io::Write;

use crate::color;
use crate::error::Result;
use crate::proc::{Invocation, Spawner};
use crate::tool_registry::{current_platform_key, locate_tool, ToolRegistry};

use super::{io, tool_status::tool_available_on_platform};

pub(super) fn doctor<S: Spawner>(
    reg: &ToolRegistry,
    id: Option<&str>,
    run_check: bool,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let platform = current_platform_key();
    if let Some(id) = id {
        return doctor_one(reg, id, platform, run_check, spawner, out);
    }

    write_header(platform, out)?;
    let mut issues = 0;
    for (id, tool) in &reg.tools {
        let loc = locate_tool(tool);
        if loc.found {
            writeln!(
                out,
                "  {green}✓{reset} {} — {} ({})",
                tool.display_name,
                loc.source,
                loc.path.display(),
                green = color::green(),
                reset = color::reset()
            )
            .map_err(io)?;
        } else {
            let available = tool_available_on_platform(tool, platform);
            if available {
                writeln!(
                    out,
                    "  {yel}-{reset} {} — not installed {dim}(pulp tool install {id}){reset}",
                    tool.display_name,
                    yel = color::yellow(),
                    reset = color::reset(),
                    dim = color::dim()
                )
                .map_err(io)?;
            } else {
                writeln!(
                    out,
                    "  {red}✗{reset} {} — not available for {platform}",
                    tool.display_name,
                    red = color::red(),
                    reset = color::reset()
                )
                .map_err(io)?;
                issues += 1;
            }
        }
    }
    Ok(i32::from(issues > 0))
}

fn doctor_one<S: Spawner>(
    reg: &ToolRegistry,
    id: &str,
    platform: &str,
    run_check: bool,
    spawner: &S,
    out: &mut impl Write,
) -> Result<i32> {
    let Some(tool) = reg.tools.get(id) else {
        writeln!(
            out,
            "{red}✗{reset} Tool '{id}' not found",
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    };

    write_header(platform, out)?;
    if !tool_available_on_platform(tool, platform) {
        writeln!(
            out,
            "  {red}✗{reset} {} — not available for {platform}",
            tool.display_name,
            red = color::red(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(1);
    }

    let loc = locate_tool(tool);
    if !loc.found {
        writeln!(
            out,
            "  {red}✗{reset} {} — not installed {dim}(pulp tool install {id}){reset}",
            tool.display_name,
            red = color::red(),
            reset = color::reset(),
            dim = color::dim()
        )
        .map_err(io)?;
        return Ok(1);
    }

    writeln!(
        out,
        "  {green}✓{reset} {} — {} ({})",
        tool.display_name,
        loc.source,
        loc.path.display(),
        green = color::green(),
        reset = color::reset()
    )
    .map_err(io)?;

    if !run_check {
        if tool.install_method == "npm_package" {
            writeln!(
                out,
                "  {dim}Run `pulp tool doctor {id} --run` to execute its smoke check.{reset}",
                dim = color::dim(),
                reset = color::reset()
            )
            .map_err(io)?;
        }
        return Ok(0);
    }

    let inv = Invocation::new(loc.path.to_string_lossy().into_owned());
    let rc = spawner.run(&inv)?;
    if rc == 0 {
        writeln!(
            out,
            "  {green}✓{reset} {} smoke check passed",
            tool.display_name,
            green = color::green(),
            reset = color::reset()
        )
        .map_err(io)?;
        return Ok(0);
    }

    writeln!(
        out,
        "{red}✗{reset} {} smoke check failed with exit code {rc}",
        tool.display_name,
        red = color::red(),
        reset = color::reset()
    )
    .map_err(io)?;
    Ok(rc)
}

fn write_header(platform: &str, out: &mut impl Write) -> Result<()> {
    writeln!(
        out,
        "Tool Health {dim}({platform}){reset}:\n",
        dim = color::dim(),
        reset = color::reset()
    )
    .map_err(io)
}
