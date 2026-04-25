//! Criterion benchmarks for the prototype's hot paths.
//!
//! # What we measure
//!
//! | Benchmark            | Question it answers                              |
//! |----------------------|--------------------------------------------------|
//! | `parse_pulp_toml_1kb`| How fast does the TOML parser ingest one file?   |
//! | `parse_semver`       | How fast is `SemverCompat::parse`?               |
//! | `emit_json_small`    | JSON writer throughput on a tiny diag snapshot.  |
//! | `compose_findings_100_projects` | Finding engine with a fat registry.   |
//!
//! # How to run
//!
//! ```bash
//! cargo bench --bench hot_paths
//! ```
//!
//! Criterion writes HTML reports under
//! `target/criterion/hot_paths/report/index.html`.

use criterion::{black_box, criterion_group, criterion_main, Criterion};

use pulp_rs::diag::{analyze, Inputs, ProjectEntry};
use pulp_rs::parse::{PulpToml, SemverCompat};

fn parse_pulp_toml_1kb(c: &mut Criterion) {
    // Roughly 1 KB of realistic pulp.toml content.
    let body: String = {
        let mut s = String::new();
        s.push_str("sdk_version = \"0.38.0\"\n");
        s.push_str("cli_min_version = \"0.37.0\"\n");
        s.push_str("\n[build]\nformats = [\"VST3\", \"AU\", \"CLAP\"]\n");
        s.push_str("\n[plugin]\nname = \"ExamplePlugin\"\n");
        // Pad out with commentary + empty sections until we're ≥1024B.
        while s.len() < 1024 {
            s.push_str("# filler line to exercise parser throughput\n");
        }
        s
    };
    c.bench_function("parse_pulp_toml_1kb", |b| {
        b.iter(|| {
            let t = PulpToml::parse_body(black_box(&body)).unwrap();
            black_box(t.sdk_version().map(str::to_owned));
        });
    });
}

fn parse_semver(c: &mut Criterion) {
    c.bench_function("parse_semver_clean_triple", |b| {
        b.iter(|| black_box(SemverCompat::parse(black_box("0.38.12"))));
    });
    c.bench_function("parse_semver_prerelease", |b| {
        b.iter(|| black_box(SemverCompat::parse(black_box("0.38.0-dev"))));
    });
}

fn compose_findings_100_projects(c: &mut Criterion) {
    let cli = SemverCompat::parse("0.38.0");
    let empty = SemverCompat::default();
    let projects: Vec<ProjectEntry> = (0..100)
        .map(|i| ProjectEntry {
            path: format!("/tmp/proj-{i}"),
            name: format!("Proj{i}"),
            sdk: SemverCompat::parse(if i % 3 == 0 { "0.40.0" } else { "0.37.0" }),
            cli_min: SemverCompat::parse("0.37.0"),
            missing_on_disk: i % 7 == 0,
            scanned: false,
        })
        .collect();

    c.bench_function("compose_findings_100_projects", |b| {
        b.iter(|| {
            let inputs = Inputs {
                cli: &cli,
                plugin_min_cli: &empty,
                project_sdk: &cli,
                project_cli_min: &empty,
                projects: black_box(&projects),
            };
            black_box(analyze(&inputs));
        });
    });
}

fn emit_json_small(c: &mut Criterion) {
    use pulp_rs::VersionDiag;
    let diag = VersionDiag {
        cli: SemverCompat::parse("0.38.0"),
        project_sdk: SemverCompat::parse("0.38.0"),
        ..Default::default()
    };
    c.bench_function("emit_json_empty_diag", |b| {
        b.iter(|| black_box(pulp_rs::emit_json(black_box(&diag))));
    });
}

criterion_group!(
    benches,
    parse_pulp_toml_1kb,
    parse_semver,
    compose_findings_100_projects,
    emit_json_small
);
criterion_main!(benches);
