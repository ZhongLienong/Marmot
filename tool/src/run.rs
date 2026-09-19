//! `marmot run`: marmotc builds the program into the project's `target/`, and
//! marmotvm runs it.

use crate::manifest;
use crate::paths;
use crate::plan::Plan;
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Output};

/// The VM: `--marmotvm`, then MARMOTVM, then marmotvm beside the compiler or
/// beside this program, then marmotvm on PATH.
pub fn find_vm(explicit: Option<&Path>, compiler: &Path) -> PathBuf {
    if let Some(path) = explicit {
        return path.to_path_buf();
    }
    if let Some(path) = std::env::var_os("MARMOTVM").filter(|value| !value.is_empty()) {
        return PathBuf::from(path);
    }

    let name = format!("marmotvm{}", std::env::consts::EXE_SUFFIX);
    let beside_compiler = compiler.parent().map(|directory| directory.join(&name));
    let beside_tool = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|directory| directory.join(&name)));
    beside_compiler
        .into_iter()
        .chain(beside_tool)
        .find(|candidate| candidate.is_file())
        .unwrap_or_else(|| PathBuf::from("marmotvm"))
}

/// A directory removed when dropped.
pub struct TemporaryDirectory(PathBuf);

impl TemporaryDirectory {
    pub fn new(path: PathBuf) -> TemporaryDirectory {
        TemporaryDirectory(path)
    }
}

impl Drop for TemporaryDirectory {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Where the program built from `entry` goes: in a project, `target/` at its
/// root, mirroring the entry's path in the project (`src/Main.mmt` becomes
/// `target/src/Main.mmc`); outside one, a temporary directory that lives as
/// long as the returned guard.
pub fn program_path(entry: &Path) -> Result<(PathBuf, Option<TemporaryDirectory>), String> {
    let entry = paths::absolute(entry);
    let directory = entry.parent().map(Path::to_path_buf).unwrap_or_default();
    let mmc = entry.with_extension("mmc");
    let file_name = mmc.file_name().map(PathBuf::from).unwrap_or_default();

    if let Some(workspace) = manifest::find_workspace(&directory)? {
        let relative = mmc
            .strip_prefix(&workspace.root)
            .map(Path::to_path_buf)
            .unwrap_or(file_name);
        return Ok((workspace.root.join("target").join(relative), None));
    }

    let temporary = std::env::temp_dir().join(format!("marmot-run-{}", std::process::id()));
    Ok((
        temporary.join(file_name),
        Some(TemporaryDirectory(temporary)),
    ))
}

fn exit_code(code: Option<i32>) -> ExitCode {
    code.map(|code| ExitCode::from((code & 0xFF) as u8))
        .unwrap_or(ExitCode::FAILURE)
}

fn output_of(command: &mut Command, program: &Path) -> Result<Output, String> {
    command
        .output()
        .map_err(|error| format!("cannot run {}: {error}", program.display()))
}

fn json_of(output: &Output, program: &Path) -> Result<Value, String> {
    serde_json::from_slice(&output.stdout).map_err(|error| {
        format!(
            "{} did not report in JSON ({error}):\n{}{}",
            program.display(),
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        )
    })
}

fn array_of(report: &Value, member: &str) -> Vec<Value> {
    report
        .get(member)
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default()
}

/// One `run` payload from the build's and the run's: the build's warnings, the
/// run's output and errors.
fn merged(build: &Value, run: &Value) -> Value {
    let build_report = &build["report"];
    let run_report = &run["report"];
    let mut diagnostics = array_of(build_report, "diagnostics");
    diagnostics.extend(array_of(run_report, "diagnostics"));

    let mut payload = run.clone();
    payload["source"] = Value::from("marmot");
    payload["report"] = serde_json::json!({
        "version": 1,
        "source": "marmot",
        "diagnostics": diagnostics,
        "warnings": array_of(build_report, "warnings"),
        "errors": array_of(run_report, "errors"),
    });
    payload
}

pub struct RunRequest<'a> {
    pub plan: &'a Plan,
    pub plan_file: &'a Path,
    pub entry: &'a Path,
    pub compiler: &'a Path,
    pub vm: &'a Path,
    pub json: bool,
}

pub fn run(request: &RunRequest) -> Result<ExitCode, String> {
    let (program, _temporary) = program_path(request.entry)?;

    let mut build = Command::new(request.compiler);
    build
        .arg("build")
        .arg("--plan")
        .arg(request.plan_file)
        .arg("-o")
        .arg(&program);

    let mut vm = Command::new(request.vm);
    vm.arg(&program);
    for library in &request.plan.native_libraries {
        vm.arg("--library")
            .arg(format!("{}={}", library.name, library.path));
    }

    if !request.json {
        let built = build
            .arg("--quiet")
            .status()
            .map_err(|error| format!("cannot run {}: {error}", request.compiler.display()))?;
        if !built.success() {
            return Ok(exit_code(built.code()));
        }
        let ran = vm
            .status()
            .map_err(|error| format!("cannot run {}: {error}", request.vm.display()))?;
        return Ok(exit_code(ran.code()));
    }

    let built = output_of(build.args(["--format", "json"]), request.compiler)?;
    let mut build_payload = json_of(&built, request.compiler)?;
    if !built.status.success() {
        build_payload["command"] = Value::from("run");
        println!("{build_payload}");
        return Ok(exit_code(built.status.code()));
    }

    let ran = output_of(vm.args(["--format", "json"]), request.vm)?;
    let run_payload = json_of(&ran, request.vm)?;
    println!("{}", merged(&build_payload, &run_payload));
    Ok(exit_code(ran.status.code()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_run_report_keeps_the_builds_warnings_and_the_runs_errors() {
        let build = serde_json::json!({
            "command": "build",
            "report": { "diagnostics": [{ "w": 1 }], "warnings": [{ "w": 1 }], "errors": [] }
        });
        let run = serde_json::json!({
            "source": "marmotvm", "command": "run", "success": false, "exitCode": 1,
            "stdout": "out", "stderr": "",
            "report": { "diagnostics": [{ "e": 1 }], "warnings": [], "errors": [{ "e": 1 }] }
        });

        let payload = merged(&build, &run);

        assert_eq!(payload["source"], "marmot");
        assert_eq!(payload["command"], "run");
        assert_eq!(payload["stdout"], "out");
        assert_eq!(payload["exitCode"], 1);
        assert_eq!(
            payload["report"]["diagnostics"],
            serde_json::json!([{ "w": 1 }, { "e": 1 }])
        );
        assert_eq!(
            payload["report"]["warnings"],
            serde_json::json!([{ "w": 1 }])
        );
        assert_eq!(payload["report"]["errors"], serde_json::json!([{ "e": 1 }]));
    }
}
