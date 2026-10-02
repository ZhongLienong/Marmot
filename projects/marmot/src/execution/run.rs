//! `marmot run`: marmotc builds the program into the project's `target/`, and
//! marmotvm runs it.

use super::cache;
use super::plan::Plan;
use super::temporary::TemporaryDirectory;
use crate::paths;
use crate::project::manifest;
use serde_json::Value;
use std::num::NonZeroUsize;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Output};

/// Where the program built from `entry` goes: in a project, `target/` at its
/// root, mirroring the entry's path in the project (`src/Main.mmt` becomes
/// `target/src/Main.mmc`); outside one, a temporary directory that lives as
/// long as the returned guard.
pub(crate) fn program_path(entry: &Path) -> Result<(PathBuf, Option<TemporaryDirectory>), String> {
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
        Some(TemporaryDirectory::new(temporary)),
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

pub(crate) struct RunRequest<'a> {
    pub(crate) plan: &'a Plan,
    pub(crate) plan_file: &'a Path,
    pub(crate) entry: &'a Path,
    pub(crate) compiler: &'a Path,
    pub(crate) vm: &'a Path,
    pub(crate) json: bool,
    /// Build even when the program in target/ is still current.
    pub(crate) rebuild: bool,
    pub(crate) jobs: Option<NonZeroUsize>,
    pub(crate) timings: bool,
}

/// The build's report when it was skipped: the stamp replays it, and an older
/// stamp that recorded none reports nothing.
fn replayed_report(stamp: &cache::Stamp) -> Value {
    stamp.report.clone().unwrap_or_else(|| {
        serde_json::json!({
            "version": 1, "source": "marmot",
            "diagnostics": [], "warnings": [], "errors": [],
        })
    })
}

/// Records what the build produced, so the next run can skip it and still say
/// what the build said.
fn remember(
    request: &RunRequest,
    program: &Path,
    plan_json: &str,
    output: Option<String>,
    report: Option<Value>,
) -> Result<(), String> {
    let deps = cache::deps_path(program);
    let files = cache::read_deps(&deps)?;
    let _ = std::fs::remove_file(&deps);
    let stamp = cache::stamp(plan_json, request.compiler, &files, output, report)?;
    cache::write(program, &stamp)
}

pub(crate) fn run(request: &RunRequest) -> Result<ExitCode, String> {
    let (program, _temporary) = program_path(request.entry)?;
    let plan_json = request.plan.to_json();
    // A stamp replays one form of what the build said, so it serves the mode it
    // was recorded in; the other mode builds once and records its own.
    let current = if request.rebuild || request.timings {
        None
    } else {
        cache::fresh(&program, &plan_json, request.compiler).filter(|stamp| {
            if request.json {
                stamp.report.is_some()
            } else {
                stamp.output.is_some()
            }
        })
    };

    let mut build = Command::new(request.compiler);
    build
        .arg("build")
        .arg("--plan")
        .arg(request.plan_file)
        .arg("-o")
        .arg(&program)
        .arg("--deps")
        .arg(cache::deps_path(&program));
    if let Some(jobs) = request.jobs {
        build.arg("--jobs").arg(jobs.to_string());
    }
    if request.timings {
        build.arg("--timings");
    }

    let mut vm = Command::new(request.vm);
    vm.arg("run").arg(&program);
    for library in &request.plan.native_libraries {
        vm.arg("--library")
            .arg(format!("{}={}", library.name, library.path));
    }

    if !request.json {
        match &current {
            Some(stamp) => print!("{}", stamp.output.clone().unwrap_or_default()),
            None => {
                let built = output_of(build.arg("--quiet"), request.compiler)?;
                print!("{}", String::from_utf8_lossy(&built.stdout));
                eprint!("{}", String::from_utf8_lossy(&built.stderr));
                if !built.status.success() {
                    return Ok(exit_code(built.status.code()));
                }
                remember(
                    request,
                    &program,
                    &plan_json,
                    Some(String::from_utf8_lossy(&built.stdout).into_owned()),
                    None,
                )?;
            }
        }

        let ran = vm
            .status()
            .map_err(|error| format!("cannot run {}: {error}", request.vm.display()))?;
        return Ok(exit_code(ran.code()));
    }

    let mut build_payload = match &current {
        Some(stamp) => serde_json::json!({ "report": replayed_report(stamp) }),
        None => {
            let built = output_of(build.args(["--format", "json"]), request.compiler)?;
            eprint!("{}", String::from_utf8_lossy(&built.stderr));
            let mut payload = json_of(&built, request.compiler)?;
            if !built.status.success() {
                payload["command"] = Value::from("run");
                println!("{payload}");
                return Ok(exit_code(built.status.code()));
            }
            remember(
                request,
                &program,
                &plan_json,
                None,
                Some(payload["report"].clone()),
            )?;
            payload
        }
    };
    build_payload["command"] = Value::from("run");

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
