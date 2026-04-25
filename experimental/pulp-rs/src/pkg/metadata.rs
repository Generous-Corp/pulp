//! Update `DEPENDENCIES.md` + `NOTICE.md` when packages are added /
//! removed.
//!
//! # Behaviour parity with C++
//!
//! The C++ reference (`package_commands.cpp::update_dependencies_md`,
//! `update_notice_md`) uses alphabetical insertion inside the existing
//! Markdown table for `DEPENDENCIES.md`, and alphabetical `## Name`
//! insertion for `NOTICE.md`. Removals simply delete the matching row
//! or section.
//!
//! # Edge cases kept in parity
//!
//! - If the file is empty / missing we no-op (the C++ writer returns
//!   early when `read_file` is empty).
//! - The registry's `last_verified` date is *not* re-read — the C++
//!   writer hard-codes "2026-04-07" in the row. We match that literal
//!   string so parity tests stay stable.
//! - The `NOTICE.md` removal path strips the leading blank line above
//!   the section header, matching the C++ behaviour.

use std::fs;
use std::path::Path;

use crate::error::{CliError, Result};
use crate::pkg::registry::PackageDescriptor;

/// Date stamp used in new `DEPENDENCIES.md` rows. Matches the C++
/// literal (`package_commands.cpp::update_dependencies_md`).
pub const DEPENDENCIES_DATE: &str = "2026-04-07";

/// Insert or remove the row for `pkg` in `<root>/DEPENDENCIES.md`.
///
/// No-op when the file doesn't exist or is empty.
///
/// # Errors
///
/// Returns [`CliError::Io`] on write failure.
pub fn update_dependencies_md(root: &Path, pkg: &PackageDescriptor, add: bool) -> Result<()> {
    let path = root.join("DEPENDENCIES.md");
    let Ok(content) = fs::read_to_string(&path) else {
        return Ok(());
    };
    if content.is_empty() {
        return Ok(());
    }
    let entry = format!(
        "| {} | {} | {} | FetchContent | {} | {} |",
        pkg.name, pkg.version, pkg.license, pkg.description, DEPENDENCIES_DATE
    );
    let new_body = if add {
        insert_row(&content, &pkg.name, &entry)
    } else {
        remove_row(&content, &pkg.name)
    };
    fs::write(&path, new_body).map_err(|e| CliError::io(path, e))
}

/// Insert or remove the section for `pkg` in `<root>/NOTICE.md`.
///
/// # Errors
///
/// Returns [`CliError::Io`] on write failure.
pub fn update_notice_md(root: &Path, pkg: &PackageDescriptor, add: bool) -> Result<()> {
    let path = root.join("NOTICE.md");
    let content = fs::read_to_string(&path).unwrap_or_default();

    let new_body = if add {
        let block = format!("\n## {}\n\n{} — {}\n", pkg.name, pkg.license, pkg.url);
        insert_notice_section(&content, &pkg.name, &block)
    } else {
        remove_notice_section(&content, &pkg.name)
    };
    fs::write(&path, new_body).map_err(|e| CliError::io(path, e))
}

fn insert_row(content: &str, name: &str, entry: &str) -> String {
    let mut out = String::with_capacity(content.len() + entry.len() + 4);
    let mut inserted = false;
    let mut in_table = false;
    for line in content.lines() {
        if line.contains("| Name") || line.starts_with("|---") {
            in_table = true;
            out.push_str(line);
            out.push('\n');
            continue;
        }
        if in_table && !inserted && line.starts_with("| ") {
            if let Some(row_name) = extract_first_cell(line) {
                if name < row_name.as_str() {
                    out.push_str(entry);
                    out.push('\n');
                    inserted = true;
                }
            }
        }
        if in_table && !inserted && !line.starts_with("| ") {
            out.push_str(entry);
            out.push('\n');
            inserted = true;
        }
        out.push_str(line);
        out.push('\n');
    }
    if !inserted {
        out.push_str(entry);
        out.push('\n');
    }
    out
}

fn extract_first_cell(line: &str) -> Option<String> {
    // Line starts with "| " already; find the next " |" pair.
    let rest = &line[2..];
    let end = rest.find(" |")?;
    Some(rest[..end].trim().to_owned())
}

fn remove_row(content: &str, name: &str) -> String {
    let needle = format!("| {name} |");
    content.lines().filter(|line| !line.contains(&needle)).fold(
        String::with_capacity(content.len()),
        |mut acc, line| {
            acc.push_str(line);
            acc.push('\n');
            acc
        },
    )
}

fn insert_notice_section(content: &str, name: &str, block: &str) -> String {
    // Walk `## Headers` in order, inserting block before the first
    // header whose name sorts after `name`.
    let mut search_from = 0;
    while let Some(pos_rel) = content[search_from..].find("## ") {
        let pos = search_from + pos_rel;
        // Consider only headers at line start.
        let line_start = content[..pos].rfind('\n').map_or(0, |i| i + 1);
        if pos != line_start {
            search_from = pos + 3;
            continue;
        }
        let end_of_line = content[pos..].find('\n').map_or(content.len(), |i| pos + i);
        let section_name = &content[pos + 3..end_of_line];
        if name < section_name {
            let mut out = String::with_capacity(content.len() + block.len() + 1);
            out.push_str(&content[..pos]);
            out.push_str(block);
            out.push('\n');
            out.push_str(&content[pos..]);
            return out;
        }
        search_from = end_of_line + 1;
    }
    let mut out = String::from(content);
    out.push_str(block);
    out
}

fn remove_notice_section(content: &str, name: &str) -> String {
    let header = format!("## {name}");
    let Some(pos) = find_header_at_line_start(content, &header) else {
        return content.to_owned();
    };
    let next = content[pos + 1..]
        .find("\n## ")
        .map_or(content.len(), |i| pos + 1 + i);
    // Drop the leading blank line before the header, if any.
    let start = if pos > 0 && content.as_bytes()[pos - 1] == b'\n' {
        pos - 1
    } else {
        pos
    };
    let mut out = String::with_capacity(content.len());
    out.push_str(&content[..start]);
    out.push_str(&content[next..]);
    out
}

fn find_header_at_line_start(content: &str, header: &str) -> Option<usize> {
    let mut search_from = 0;
    while let Some(pos_rel) = content[search_from..].find(header) {
        let pos = search_from + pos_rel;
        let line_start = content[..pos].rfind('\n').map_or(0, |i| i + 1);
        if pos == line_start {
            // Must be exactly the header — avoid matching `## FooBar`
            // when looking for `## Foo`.
            let after = &content[pos + header.len()..];
            if after.starts_with('\n') || after.is_empty() {
                return Some(pos);
            }
        }
        search_from = pos + header.len();
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn sample_pkg() -> PackageDescriptor {
        PackageDescriptor {
            id: "libX".to_owned(),
            name: "LibX".to_owned(),
            version: "1.0.0".to_owned(),
            description: "An X library".to_owned(),
            license: "MIT".to_owned(),
            url: "https://example.com/libx".to_owned(),
            ..PackageDescriptor::default()
        }
    }

    const BASE_DEPS_MD: &str = "# Dependencies

| Name | Version | License | Bundled | Purpose | Added |
|------|---------|---------|---------|---------|-------|
| Alpha | 1.0 | MIT | FetchContent | Alpha lib | 2026-01-01 |
| Zulu | 1.0 | MIT | FetchContent | Zulu lib | 2026-01-01 |
";

    const BASE_NOTICE_MD: &str = "# Notice

## Alpha

MIT — https://example.com/alpha

## Zulu

MIT — https://example.com/zulu
";

    #[test]
    fn dep_row_inserts_alphabetically() {
        let td = tempfile::tempdir().unwrap();
        fs::write(td.path().join("DEPENDENCIES.md"), BASE_DEPS_MD).unwrap();
        update_dependencies_md(td.path(), &sample_pkg(), true).unwrap();
        let body = fs::read_to_string(td.path().join("DEPENDENCIES.md")).unwrap();
        let lines: Vec<&str> = body.lines().collect();
        let alpha = lines.iter().position(|l| l.contains("| Alpha |")).unwrap();
        let libx = lines.iter().position(|l| l.contains("| LibX |")).unwrap();
        let zulu = lines.iter().position(|l| l.contains("| Zulu |")).unwrap();
        assert!(alpha < libx && libx < zulu);
    }

    #[test]
    fn dep_row_remove_drops_line() {
        let td = tempfile::tempdir().unwrap();
        fs::write(td.path().join("DEPENDENCIES.md"), BASE_DEPS_MD).unwrap();
        let mut pkg = sample_pkg();
        pkg.name = "Zulu".to_owned();
        update_dependencies_md(td.path(), &pkg, false).unwrap();
        let body = fs::read_to_string(td.path().join("DEPENDENCIES.md")).unwrap();
        assert!(!body.contains("| Zulu |"));
        assert!(body.contains("| Alpha |"));
    }

    #[test]
    fn dep_row_no_op_when_file_missing() {
        let td = tempfile::tempdir().unwrap();
        // No file planted — function returns Ok and does nothing.
        update_dependencies_md(td.path(), &sample_pkg(), true).unwrap();
        assert!(!td.path().join("DEPENDENCIES.md").exists());
    }

    #[test]
    fn notice_section_inserts_alphabetically() {
        let td = tempfile::tempdir().unwrap();
        fs::write(td.path().join("NOTICE.md"), BASE_NOTICE_MD).unwrap();
        update_notice_md(td.path(), &sample_pkg(), true).unwrap();
        let body = fs::read_to_string(td.path().join("NOTICE.md")).unwrap();
        let alpha = body.find("## Alpha").unwrap();
        let libx = body.find("## LibX").unwrap();
        let zulu = body.find("## Zulu").unwrap();
        assert!(alpha < libx && libx < zulu);
        assert!(body.contains("MIT — https://example.com/libx"));
    }

    #[test]
    fn notice_section_remove_drops_block() {
        let td = tempfile::tempdir().unwrap();
        fs::write(td.path().join("NOTICE.md"), BASE_NOTICE_MD).unwrap();
        let mut pkg = sample_pkg();
        pkg.name = "Alpha".to_owned();
        update_notice_md(td.path(), &pkg, false).unwrap();
        let body = fs::read_to_string(td.path().join("NOTICE.md")).unwrap();
        assert!(!body.contains("## Alpha"));
        assert!(body.contains("## Zulu"));
    }

    #[test]
    fn notice_section_append_when_no_earlier_match() {
        let td = tempfile::tempdir().unwrap();
        fs::write(
            td.path().join("NOTICE.md"),
            "# Notice\n\n## Alpha\n\nMIT — x\n",
        )
        .unwrap();
        let mut pkg = sample_pkg();
        pkg.name = "ZZZ".to_owned();
        update_notice_md(td.path(), &pkg, true).unwrap();
        let body = fs::read_to_string(td.path().join("NOTICE.md")).unwrap();
        let alpha = body.find("## Alpha").unwrap();
        let zzz = body.find("## ZZZ").unwrap();
        assert!(alpha < zzz);
    }

    #[test]
    fn notice_section_handles_prefix_match_safely() {
        // Removing `LibX` must not accidentally match `LibXYZ`.
        let td = tempfile::tempdir().unwrap();
        fs::write(
            td.path().join("NOTICE.md"),
            "## LibX\n\nMIT\n\n## LibXYZ\n\nMIT\n",
        )
        .unwrap();
        let mut pkg = sample_pkg();
        pkg.name = "LibX".to_owned();
        update_notice_md(td.path(), &pkg, false).unwrap();
        let body = fs::read_to_string(td.path().join("NOTICE.md")).unwrap();
        assert!(!body.contains("## LibX\n"));
        assert!(body.contains("## LibXYZ"));
    }
}
