#!/usr/bin/env python3
"""Verify the allpass from an installed SDK and retain a digest-addressed proof bundle.

The consumer is copied from the installed example to a temporary directory,
never compiled against this checkout. This driver is checkout-side evidence
orchestration; its schema validator and Quality Lab are measurement tools.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


SEARCH_ENV = {
    "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "CPATH", "CFLAGS", "CXXFLAGS",
    "LDFLAGS", "LIBRARY_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH", "OBJC_INCLUDE_PATH", "OBJCPLUS_INCLUDE_PATH",
    "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "CMAKE_PREFIX_PATH", "Pulp_DIR",
}
RENDERS = (
    "source-regular", "source-irregular", "reload-regular", "reload-irregular",
    "bake-regular", "bake-irregular",
)


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def environment() -> dict[str, str]:
    return {key: value for key, value in os.environ.items() if key not in SEARCH_ENV}


def run(command: list[str], log: Path, *, cwd: Path, env: dict[str, str]) -> None:
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write_text(result.stdout)
    require(result.returncode == 0,
            f"command failed ({result.returncode}); inspect {log}: {command[0]}")


def validate_catalogs(sdk: Path, output: Path) -> dict[str, str]:
    validator_path = Path(__file__).resolve().parents[1] / "scripts/json_schema_lite.py"
    spec = importlib.util.spec_from_file_location("sample_region_schema_validator", validator_path)
    require(spec is not None and spec.loader is not None, "schema validator unavailable")
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    share = sdk / "share/pulp"
    hashes = {}
    for stem in ("agent-capabilities", "dsp-capabilities", "forge-catalog"):
        document_path, schema_path = share / f"{stem}.json", share / f"{stem}.schema.json"
        require(document_path.is_file() and schema_path.is_file(),
                f"installed {stem} document/schema missing")
        document, schema = json.loads(document_path.read_text()), json.loads(schema_path.read_text())
        problems = validator.validate(document, schema)
        require(not problems, f"installed {stem} schema failed: {problems}")
        for path in (document_path, schema_path):
            shutil.copy2(path, output / path.name)
            hashes[path.name] = digest(path)
        if stem == "agent-capabilities":
            rows = {row["key"]: row for row in document["capabilities"]}
            for key in ("signal.unit-delay", "signal.sample-region"):
                require(key in rows, f"installed manifest omits {key}")
                require(rows[key]["status"] == "experimental", f"unexpected status for {key}")
    return hashes


def verify_sdk_identity(sdk: Path, output: Path, source_sha: str, sdk_platform: str) -> str:
    # Reuse the release authority's complete schema/importer/manifest verification.
    scripts = Path(__file__).resolve().parents[1] / "scripts"
    sys.path.insert(0, str(scripts))
    try:
        from sdk_capability_handoff import verify_handoff
        from sdk_provenance import _importer_runtime_paths
        verify_handoff(sdk, expected_sdk_source_sha=source_sha,
                       expected_platform=sdk_platform,
                       expected_importer_runtime_paths=_importer_runtime_paths(sdk, sdk_platform))
    finally:
        sys.path.pop(0)
    handoff_path = sdk / "share/pulp/agent-capability-handoff.json"
    shutil.copy2(handoff_path, output / handoff_path.name)
    inventory = {}
    for path in sorted(sdk.rglob("*")):
        relative = path.relative_to(sdk).as_posix()
        if path.is_symlink():
            resolved = path.resolve(strict=True)
            require(resolved.is_relative_to(sdk), f"SDK symlink escapes installation: {relative}")
            inventory[relative] = {"kind": "symlink", "target": os.readlink(path)}
        elif path.is_file():
            before = path.stat()
            value = digest(path)
            after = path.stat()
            require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns),
                    f"SDK changed during inventory: {relative}")
            inventory[relative] = {"kind": "file", "bytes": after.st_size, "sha256": value}
    require(bool(inventory), "empty SDK inventory")
    write_json(output / "sdk-inventory.json", inventory)
    return digest(output / "sdk-inventory.json")


def request_file_api(build: Path) -> None:
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True, exist_ok=True)
    for kind in ("codemodel-v2", "toolchains-v1"):
        (query / kind).write_text("")


def file_api_inputs(project: Path, target_name: str) -> tuple[dict, Path, list[Path]]:
    reply = project / "build/.cmake/api/v1/reply"
    indexes = sorted(reply.glob("index-*.json"))
    require(bool(indexes), "CMake File API reply is missing")
    index = json.loads(indexes[-1].read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    configurations = [v for v in model["configurations"] if v["name"] == "Release"]
    require(len(configurations) == 1, "expected exactly one Release codemodel")
    targets = [v for v in configurations[0]["targets"] if v["name"] == target_name]
    require(len(targets) == 1, f"expected exactly one {target_name} codemodel target")
    target_path = reply / targets[0]["jsonFile"]
    toolchains = json.loads((reply / index["reply"]["toolchains-v1"]["jsonFile"]).read_text())
    system_roots = []
    for toolchain in toolchains["toolchains"]:
        implicit = toolchain.get("compiler", {}).get("implicit", {})
        for kind in ("includeDirectories", "linkDirectories", "linkFrameworkDirectories"):
            system_roots.extend(Path(p).resolve() for p in implicit.get(kind, []))
    require(bool(system_roots), "compiler reported no implicit system directories")
    return json.loads(target_path.read_text()), target_path, system_roots


def inspect_strict_inputs(sdk: Path, project: Path, target_name: str,
                          forbidden: list[Path], *, canonical: bool = True,
                          own_roots: tuple[Path, Path] | None = None) -> dict:
    import shlex
    build = own_roots[1] if own_roots else project / "build"
    source = own_roots[0] if own_roots else project
    if canonical:
        scripts = Path(__file__).resolve().parents[1] / "scripts"
        sys.path.insert(0, str(scripts))
        try:
            from test_agent_capability_installed_sdk import inspect_isolation
            inspect_isolation(sdk, project, "Release", forbidden, target_name,
                              allowed_source_roots=(project, build))
        finally:
            sys.path.pop(0)
    target, _target_path, system_roots = file_api_inputs(project, target_name)
    roots = [sdk.resolve(), source.resolve(), build.resolve(), *system_roots]
    forbidden = [p.resolve() for p in forbidden]

    def check_path(spelling: str, *, sysroot: bool = False) -> None:
        path = Path(spelling)
        path = (path if path.is_absolute() else build / path).resolve()
        require(not any(path.is_relative_to(root) for root in forbidden),
                f"consumer input leaks forbidden checkout: {spelling}")
        permitted = any(path.is_relative_to(root) for root in roots)
        if sysroot:
            permitted = permitted or any(root.is_relative_to(path) for root in system_roots)
        require(permitted, f"consumer input escapes installed/external/toolchain roots: {spelling}")

    includes = [v for group in target.get("compileGroups", []) for v in group.get("includes", [])]
    for include in includes:
        check_path(include["path"])
    fragments = [v["fragment"] for group in target.get("compileGroups", [])
                 for v in group.get("compileCommandFragments", [])]
    fragments += [v["fragment"] for v in target.get("link", {}).get("commandFragments", [])]
    for fragment in fragments:
        tokens = shlex.split(fragment)
        expanded = []
        for token in tokens:
            expanded.extend(token[4:].split(",") if token.startswith("-Wl,") else [token])
        pending = None
        for token in expanded:
            if pending:
                check_path(token, sysroot=pending in {"-isysroot", "--sysroot"})
                pending = None
                continue
            if token in {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros",
                         "-L", "-F", "-isysroot", "--sysroot", "-T", "--script"}:
                pending = token
                continue
            prefix = next((p for p in ("-isystem", "-iquote", "-idirafter", "--sysroot=",
                                      "-I", "-L", "-F", "/LIBPATH:")
                           if token.startswith(p) and len(token) > len(p)), None)
            if prefix:
                check_path(token[len(prefix):], sysroot=prefix == "--sysroot=")
            elif token.startswith("@") and not token.startswith(("@rpath/", "@loader_path/", "@executable_path/")):
                raise RuntimeError("unexpected response-file input in evaluated link command")
            elif not token.startswith("-") and (Path(token).is_absolute() or
                    token.endswith((".a", ".so", ".dylib", ".lib", ".o", ".obj"))):
                check_path(token)
        require(pending is None, "incomplete path-bearing compiler/linker flag")
    return {"target": target_name, "include_paths": [v["path"] for v in includes],
            "compile_and_link_fragments": fragments,
            "compiler_system_roots": [str(p) for p in system_roots]}


def isolation_negative_controls(sdk: Path, project: Path, target_name: str,
                                external: Path, output: Path, forbidden: list[Path]) -> None:
    # Preserve the evaluated real reply. Plant failures only in a disposable copy.
    negative = external / "isolation-negative"
    shutil.copytree(project / "build/.cmake", negative / "build/.cmake")
    target, target_path, _ = file_api_inputs(negative, target_name)
    original = target_path.read_bytes()
    foreign = external / "foreign-inputs"
    foreign.mkdir()
    (foreign / "foreign.a").write_bytes(b"planted foreign archive")
    results = []
    for case in ("foreign_system_include", "foreign_archive"):
        changed = json.loads(original)
        if case == "foreign_system_include":
            changed["compileGroups"][0].setdefault("includes", []).append(
                {"path": str(foreign), "isSystem": True})
        else:
            changed.setdefault("link", {}).setdefault("commandFragments", []).append(
                {"fragment": str(foreign / "foreign.a"), "role": "libraries"})
        target_path.write_text(json.dumps(changed))
        try:
            # Original allowed source/build roots are retained by inspecting the
            # copied reply as data, without blessing its separate sibling folder.
            inspect_strict_inputs(sdk, negative, target_name, forbidden, canonical=False,
                                  own_roots=(project, project / "build"))
        except (RuntimeError, AssertionError) as error:
            require("foreign-inputs" in str(error), "negative control failed for an unrelated reason")
            results.append({"case": case, "rejected": True, "message": str(error)})
        else:
            raise RuntimeError(f"isolation guard admitted {case}")
    target_path.write_bytes(original)
    write_json(output / "isolation-negative-controls.json", results)


def prove_selected_bindings(args: argparse.Namespace, sdk: Path, output: Path,
                            external: Path, config: Path, env: dict[str, str]) -> dict:
    source_root = Path(__file__).resolve().parents[2]
    source_sha = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=source_root, text=True).strip()
    require(source_sha == args.source_sha, "operational probe source is not the SDK source SHA")
    scripts = source_root / "tools/scripts"
    generator_paths = sorted(scripts.glob("agent_capability_*.py")) + [
        scripts / "test_agent_capability_installed_sdk.py"]
    changed = subprocess.check_output(
        ["git", "status", "--porcelain", "--", *[str(p.relative_to(source_root)) for p in generator_paths]],
        cwd=source_root, text=True)
    require(not changed.strip(), "operational probe generator has uncommitted source changes")
    keys = {"signal.unit-delay", "signal.sample-region"}
    installed = json.loads((sdk / "share/pulp/agent-capabilities.json").read_text())
    selected = dict(installed)
    selected["capabilities"] = [row for row in installed["capabilities"] if row["key"] in keys]
    require({row["key"] for row in selected["capabilities"]} == keys,
            "selected installed capability keys are incomplete")
    sys.path.insert(0, str(scripts))
    try:
        from test_agent_capability_installed_sdk import load_source_contract
        from agent_capability_installed_sdk_consumers import positive_consumer_proofs, write_consumer_suite
        import agent_capability_manifest
        generated = agent_capability_manifest.document(source_root)
        generated_rows = {row["key"]: row for row in generated["capabilities"] if row["key"] in keys}
        require(generated_rows == {row["key"]: row for row in selected["capabilities"]},
                "installed capabilities differ from exact probe source contract")
        probes, binding_probes, _owners, addresses = load_source_contract(source_root)
        proofs = positive_consumer_proofs(selected, probes, binding_probes, addresses)
        source = external / "binding-source"
        build = source / "build"
        write_consumer_suite(source, proofs)
    finally:
        sys.path.pop(0)
    evidence = output / "binding-proofs"
    shutil.copytree(source, evidence / "sources")
    write_json(evidence / "generator-sha256.json", {
        str(p.relative_to(source_root)): digest(p) for p in generator_paths
    })
    request_file_api(build)
    run([args.cmake, "-S", str(source), "-B", str(build),
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE",
         "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE",
         "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON", f"-DPulp_DIR={config.parent}"],
        evidence / "configure.log", cwd=external, env=env)
    isolation = [inspect_strict_inputs(sdk, source, proof.name, [source_root]) for proof in proofs]
    run([str(args.build_wrapper.resolve()), args.cmake, "--build", str(build),
         "--config", "Release", "--parallel", str(args.build_jobs)],
        evidence / "build.log", cwd=external, env=env)
    compile_commands = build / "compile_commands.json"
    commands = json.loads(compile_commands.read_text())
    require(len(commands) == len(proofs), "binding proof compile-command coverage differs")
    for command in commands:
        require(Path(command["file"]).resolve().is_relative_to(source),
                "binding proof compiled a source outside its external suite")
        spelling = command.get("command", " ".join(command.get("arguments", [])))
        require(str(source_root) not in spelling, "binding proof borrows checkout paths")
    shutil.copy2(compile_commands, evidence / "compile_commands.json")
    results = []
    for proof in proofs:
        locators = list(build.glob(f"consumer-path-{proof.name}-*.txt"))
        require(len(locators) == 1, f"ambiguous executable locator for {proof.name}")
        executable = Path(locators[0].read_text()).resolve()
        require(executable.is_file() and executable.is_relative_to(build),
                f"invalid executable path for {proof.name}")
        run([str(executable)], evidence / f"{proof.name}.log", cwd=external, env=env)
        results.append({"identity": proof.identity, "name": proof.name, "target": proof.target,
                        "source_sha256": digest(source / f"{proof.name}.cpp"),
                        "executable_sha256": digest(executable), "exit_code": 0})
        # Preserve completed proof identities even if a subsequent proof refuses.
        write_json(evidence / "results.json", results)
    write_json(evidence / "evaluated-inputs.json", isolation)
    shutil.copytree(build / ".cmake/api/v1/reply", evidence / "file-api-reply")
    return {"keys": sorted(keys), "proofs": len(proofs),
            "bindings": sum(item.identity[0] == "binding" for item in proofs),
            "aggregates": sum(item.identity[0] == "capability" for item in proofs),
            "results_sha256": digest(evidence / "results.json")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-prefix", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--sdk-platform", required=True,
                        choices=("darwin-arm64", "darwin-x64", "linux-arm64", "linux-x64",
                                 "windows-arm64", "windows-x64"))
    parser.add_argument("--quality-lab-python", type=Path, required=True)
    parser.add_argument("--quality-lab-root", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--build-jobs", type=int, default=2)
    parser.add_argument("--build-wrapper", type=Path,
                        default=Path(__file__).resolve().parents[1] / "ci/governed-build.sh")
    args = parser.parse_args()
    sdk, output = args.sdk_prefix.resolve(), args.output_dir.resolve()
    require(len(args.source_sha) == 40 and all(c in "0123456789abcdef" for c in args.source_sha),
            "source-sha must be a full lowercase Git commit SHA")
    require(args.build_jobs > 0, "build-jobs must be positive")
    require(not output.exists() or not any(output.iterdir()), "output directory must be empty")
    require(not output.is_relative_to(sdk), "evidence output must be outside SDK prefix")
    output.mkdir(parents=True, exist_ok=True)
    example = sdk / "share/pulp/examples/sample-region-allpass"
    require(example.is_dir(), "installed sample-region-allpass example missing")
    configs = list(sdk.glob("lib*/cmake/Pulp/PulpConfig.cmake"))
    require(len(configs) == 1, "SDK must contain exactly one PulpConfig.cmake")
    hashes = validate_catalogs(sdk, output)
    sdk_digest = verify_sdk_identity(sdk, output, args.source_sha, args.sdk_platform)
    env = environment()
    with tempfile.TemporaryDirectory(prefix="pulp-sample-region-installed-") as temporary:
        external = Path(temporary).resolve()
        checkout = Path(__file__).resolve().parents[2]
        require(not external.is_relative_to(checkout) and not external.is_relative_to(sdk),
                "temporary consumer directory must be outside checkout and SDK")
        binding_receipt = prove_selected_bindings(args, sdk, output, external, configs[0], env)
        source = external / "source"
        build = source / "build"
        shutil.copytree(example, source, symlinks=False)
        require(all(not p.is_symlink() for p in example.rglob("*")),
                "installed example must not borrow sources through symlinks")
        request_file_api(build)
        run([args.cmake, "-S", str(source), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
             "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE",
             "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE",
         "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
             f"-DPulp_DIR={configs[0].parent}"], output / "configure.log", cwd=external, env=env)
        evaluated = inspect_strict_inputs(sdk, source, "sample-region-allpass-consumer", [checkout])
        write_json(output / "evaluated-inputs.json", evaluated)
        shutil.copytree(build / ".cmake/api/v1/reply", output / "file-api-reply")
        isolation_negative_controls(sdk, source, "sample-region-allpass-consumer",
                                    external, output, [checkout])
        run([str(args.build_wrapper.resolve()), args.cmake, "--build", str(build), "--config", "Release", "--target",
             "sample-region-allpass-consumer", "--parallel", str(args.build_jobs)],
            output / "build.log", cwd=external, env=env)
        compile_commands = build / "compile_commands.json"
        require(compile_commands.is_file(), "consumer compile-command evidence unavailable")
        commands = json.loads(compile_commands.read_text())
        require(commands, "consumer compile-command evidence empty")
        checkout = Path(__file__).resolve().parents[2]
        for command in commands:
            require(Path(command["file"]).resolve().is_relative_to(source),
                    "consumer compiled source outside the copied installed example")
            spelling = command.get("command", " ".join(command.get("arguments", [])))
            require(str(checkout) not in spelling, "consumer compile command borrows checkout paths")
        shutil.copy2(compile_commands, output / "compile_commands.json")
        shutil.copy2(build / "CMakeCache.txt", output / "consumer-CMakeCache.txt")
        executable = build / "sample-region-allpass-consumer"
        if not executable.is_file():
            executable = build / "Release/sample-region-allpass-consumer.exe"
        require(executable.is_file(), "consumer executable missing")
        executable_hash = digest(executable)
        artifacts = output / "artifacts"
        run([str(executable), str(artifacts)], output / "consumer.log", cwd=external, env=env)
        receipt = json.loads((artifacts / "consumer-receipt.json").read_text())
        require(receipt.get("accepted") is True and receipt.get("renders") == 6,
                "consumer did not accept six render paths")
        for name in RENDERS:
            require((artifacts / f"{name}.wav").is_file(), f"missing {name} render")
        quality_env = dict(env, PYTHONPATH=str(args.quality_lab_root.resolve()))
        command = [str(args.quality_lab_python.resolve()), "-m", "quality_lab.cli", "compare",
                   str(artifacts / "source-regular.wav"), str(artifacts / "bake-regular.wav"),
                   "--profile", "tonal-balance", "--reference-role", "golden", "--align", "none",
                   "--json", str(output / "quality-lab.json")]
        run(command, output / "quality-lab.log", cwd=external, env=quality_env)
        quality = json.loads((output / "quality-lab.json").read_text())
        require(quality.get("schema") == "quality_lab.compare.v1" and
                quality.get("profile") == "tonal-balance" and
                quality.get("reference_role") == "golden", "invalid Quality Lab envelope identity")
        require(any(row.get("applicable") is True and row.get("status") == "measured"
                    for row in quality.get("measurements", [])), "Quality Lab envelope is not applicable")
        require(isinstance(quality.get("verdict"), str) and bool(quality["verdict"]),
                "Quality Lab verdict missing")
        write_json(output / "receipt.json", {
            "schema": "pulp.sample-region-installed-sdk.v1", "accepted": True,
            "source_sha": args.source_sha, "source_identity_binding": "verified installed capability handoff",
            "sdk_platform": args.sdk_platform, "sdk_inventory_sha256": sdk_digest, "sdk_prefix": str(sdk),
            "sdk_catalog_sha256": hashes, "consumer_sha256": executable_hash,
            "installed_example_sha256": {
                str(p.relative_to(example)): digest(p) for p in sorted(example.rglob("*")) if p.is_file()
            },
            "consumer_external_to_checkout": True, "consumer_receipt": receipt,
            "selected_binding_proofs": binding_receipt,
            "quality_lab_command": command, "quality_lab_verdict": quality["verdict"],
            "quality_lab_verdict_is_advisory": True,
        })
    manifest = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob("*")) if p.is_file()}
    write_json(output / "sha256.json", manifest)
    print(f"Installed allpass accepted; bundle {output}; sha256.json digest {digest(output / 'sha256.json')}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(f"sample-region installed SDK: {error}", file=sys.stderr)
        raise SystemExit(1)
