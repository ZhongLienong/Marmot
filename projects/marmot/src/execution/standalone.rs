use super::plan::Plan;
use super::temporary::{TemporaryDirectory, TemporaryFile};
use crate::paths;
use serde::Deserialize;
use serde_json::Value;
use std::fs::{self, File};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const MAGIC: &[u8; 16] = b"MARMOT-EXE-v1\0\0\0";
const FORMAT_VERSION: u32 = 1;

#[derive(Deserialize)]
struct Capability {
    version: u32,
    executable: PathBuf,
}

fn read(path: &Path) -> Result<Vec<u8>, String> {
    fs::read(path).map_err(|error| format!("cannot read {}: {error}", path.display()))
}

fn append_blob(payload: &mut Vec<u8>, bytes: &[u8]) {
    payload.extend_from_slice(&(bytes.len() as u64).to_le_bytes());
    payload.extend_from_slice(bytes);
}

fn package(
    vm: &Path,
    program: &Path,
    plan: &Plan,
    names: &[Value],
    output: &Path,
) -> Result<(), String> {
    let mut image = read(vm)?;
    let mut payload = Vec::new();
    append_blob(&mut payload, &read(program)?);
    payload.extend_from_slice(&(names.len() as u64).to_le_bytes());
    for name in names {
        let name = name
            .as_str()
            .ok_or("compiler reported an invalid native library name")?;
        let library = plan
            .native_libraries
            .iter()
            .find(|library| library.name == name)
            .ok_or_else(|| format!("native library '{name}' is absent from the build plan"))?;
        let path = Path::new(&library.path);
        let file_name = path
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or_else(|| format!("invalid native library file name: {}", path.display()))?;
        append_blob(&mut payload, name.as_bytes());
        append_blob(&mut payload, file_name.as_bytes());
        append_blob(&mut payload, &read(path)?);
    }
    image.extend_from_slice(&payload);
    image.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    image.extend_from_slice(MAGIC);

    let output = paths::absolute(output);
    let directory = output.parent().expect("an absolute file has a parent");
    fs::create_dir_all(directory)
        .map_err(|error| format!("cannot create {}: {error}", directory.display()))?;
    let staged = output.with_added_extension(format!("marmot-{}.tmp", std::process::id()));
    let mut file = File::create_new(&staged)
        .map_err(|error| format!("cannot create {}: {error}", staged.display()))?;
    let _temporary = TemporaryFile::new(staged.clone());
    file.write_all(&image)
        .map_err(|error| format!("cannot write {}: {error}", staged.display()))?;
    drop(file);
    #[cfg(unix)]
    fs::set_permissions(
        &staged,
        fs::metadata(vm)
            .map_err(|error| format!("cannot read {}: {error}", vm.display()))?
            .permissions(),
    )
    .map_err(|error| format!("cannot set permissions on {}: {error}", staged.display()))?;
    fs::rename(&staged, &output)
        .map_err(|error| format!("cannot write {}: {error}", output.display()))
}

fn print_diagnostics(payload: &Value) -> Result<(), String> {
    for diagnostic in payload["report"]["diagnostics"]
        .as_array()
        .ok_or("compiler reported invalid diagnostics")?
    {
        let severity = diagnostic["severity"]
            .as_str()
            .ok_or("compiler reported an invalid diagnostic severity")?;
        let message = diagnostic["message"]
            .as_str()
            .ok_or("compiler reported an invalid diagnostic message")?;
        if let Some(file) = diagnostic["file_path"].as_str() {
            eprint!("{file}");
            if let Some(line) = diagnostic["line"].as_u64() {
                eprint!(":{line}");
            }
            eprint!(": ");
        }
        eprintln!("{severity}: {message}");
    }
    Ok(())
}

fn packaging_failure(payload: &mut Value, message: &str) {
    payload["success"] = false.into();
    payload["exitCode"] = 1.into();
    payload.as_object_mut().unwrap().remove("artifact");
    let diagnostic = serde_json::json!({
        "source": "marmot", "severity": "error", "stage": "Compiler", "code": "None",
        "message": message, "file": null, "file_path": null, "line": null,
        "column": null, "endLine": null, "endColumn": null, "caret_length": null,
        "suggestion": null, "relatedInformation": [],
    });
    payload["report"]["diagnostics"]
        .as_array_mut()
        .unwrap()
        .push(diagnostic.clone());
    payload["report"]["errors"]
        .as_array_mut()
        .unwrap()
        .push(diagnostic);
}

fn build_program(
    mut command: Command,
    compiler: &Path,
    vm: &Path,
    plan: &Plan,
    output: &Path,
    json: bool,
) -> Result<ExitCode, String> {
    if !cfg!(any(target_os = "windows", target_os = "linux")) {
        return Err("standalone executables currently support Windows and Linux".into());
    }
    let capability = Command::new(vm)
        .arg("--standalone-version")
        .output()
        .map_err(|error| format!("cannot run {}: {error}", vm.display()))?;
    let unsupported = || {
        format!(
            "{} does not support standalone format {FORMAT_VERSION}; rebuild or reinstall marmotvm",
            vm.display()
        )
    };
    if !capability.status.success() {
        return Err(unsupported());
    }
    let capability: Capability =
        serde_json::from_slice(&capability.stdout).map_err(|_| unsupported())?;
    if capability.version != FORMAT_VERSION {
        return Err(unsupported());
    }

    let directory = std::env::temp_dir().join(format!("marmot-executable-{}", std::process::id()));
    fs::create_dir(&directory)
        .map_err(|error| format!("cannot create {}: {error}", directory.display()))?;
    let _temporary = TemporaryDirectory::new(directory.clone());
    let program = directory.join("program.mmc");
    let compiled = command
        .args(["--format", "json", "--embed-sources", "-o"])
        .arg(&program)
        .output()
        .map_err(|error| format!("cannot run {}: {error}", compiler.display()))?;
    eprint!("{}", String::from_utf8_lossy(&compiled.stderr));
    let mut payload: Value = serde_json::from_slice(&compiled.stdout)
        .map_err(|error| format!("{} reported invalid JSON: {error}", compiler.display()))?;
    let mut exit_code = compiled
        .status
        .code()
        .map(|code| ExitCode::from((code & 0xFF) as u8))
        .unwrap_or(ExitCode::FAILURE);
    if compiled.status.success() {
        let names = payload["artifact"]["nativeLibraries"]
            .as_array()
            .ok_or("compiler did not report the program's native libraries")?;
        match package(&capability.executable, &program, plan, names, output) {
            Ok(()) => {
                payload["artifact"]["path"] = paths::display(output).into();
                payload["artifact"]["kind"] = "executable".into();
            }
            Err(error) => {
                packaging_failure(&mut payload, &error);
                exit_code = ExitCode::FAILURE;
            }
        }
    }
    if json {
        println!("{payload}");
    } else {
        print_diagnostics(&payload)?;
        if exit_code == ExitCode::SUCCESS {
            println!("Built standalone executable: {}", output.display());
        }
    }
    Ok(exit_code)
}

pub(crate) fn build(
    command: Command,
    compiler: &Path,
    vm: &Path,
    plan: &Plan,
    output: &Path,
    json: bool,
) -> Result<ExitCode, String> {
    match build_program(command, compiler, vm, plan, output, json) {
        Err(error) if json => {
            let mut payload = serde_json::json!({
                "version": 1, "source": "marmot", "command": "build",
                "success": false, "exitCode": 1, "stdout": "", "stderr": "",
                "report": {
                    "version": 1, "source": "marmot",
                    "diagnostics": [], "warnings": [], "errors": [],
                },
            });
            packaging_failure(&mut payload, &error);
            println!("{payload}");
            Ok(ExitCode::FAILURE)
        }
        result => result,
    }
}
