use crate::paths;
use crate::project::version::Version;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

pub(crate) fn find_compiler(explicit: Option<&Path>) -> PathBuf {
    if let Some(path) = explicit {
        return path.to_path_buf();
    }
    if let Some(path) = std::env::var_os("MARMOTC").filter(|value| !value.is_empty()) {
        return PathBuf::from(path);
    }

    let suffix = std::env::consts::EXE_SUFFIX;
    let current = std::env::current_exe()
        .ok()
        .map(|exe| paths::identity_key(&exe));
    let siblings = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .into_iter()
        .flat_map(|directory| [format!("marmotc{suffix}")].map(|name| directory.join(name)));
    for candidate in siblings {
        if candidate.is_file() && Some(paths::identity_key(&candidate)) != current {
            return candidate;
        }
    }

    checkout_compiler().unwrap_or_else(|| PathBuf::from("marmotc"))
}

/// A debug build of this tool comes from a Marmot checkout, and wants the
/// compiler built there rather than whichever one is installed on PATH. With
/// several presets built, the one built last is the one being worked on:
/// last by either program, since a change to the VM relinks only marmotvm,
/// and the VM uses the matching preset in its own project build.
fn checkout_compiler() -> Option<PathBuf> {
    let builds = paths::checkout_root()?.join("out").join("build");
    newest_build(&builds)
}

/// The marmotc of the preset under `builds` whose marmotc or marmotvm was
/// written last.
pub(crate) fn newest_build(builds: &Path) -> Option<PathBuf> {
    let suffix = std::env::consts::EXE_SUFFIX;
    let modified = |path: PathBuf| path.metadata().ok()?.modified().ok();
    std::fs::read_dir(builds.join("marmotc"))
        .ok()?
        .filter_map(Result::ok)
        .map(|preset| preset.path().join("out"))
        .filter_map(|directory| {
            let compiler = directory.join(format!("marmotc{suffix}"));
            let compiler_modified = modified(compiler.clone())?;
            let built = modified(
                builds
                    .join("marmotvm")
                    .join(directory.parent()?.file_name()?)
                    .join("out")
                    .join(format!("marmotvm{suffix}")),
            )
            .map_or(compiler_modified, |vm_modified| {
                vm_modified.max(compiler_modified)
            });
            Some((built, compiler))
        })
        .max_by_key(|(built, _)| *built)
        .map(|(_, compiler)| compiler)
}

/// The compiler's version, from `marmotc --version` ("marmotc 1.2.3").
/// Packages declare which compiler versions they work with.
pub(crate) fn compiler_version(compiler: &Path) -> Result<Version, String> {
    let output = Command::new(compiler)
        .arg("--version")
        .output()
        .map_err(|error| format!("cannot run {}: {error}", compiler.display()))?;
    let text = String::from_utf8_lossy(&output.stdout);
    text.split_whitespace()
        .nth(1)
        .ok_or_else(|| format!("{} --version printed '{}'", compiler.display(), text.trim()))
        .and_then(|version| {
            Version::parse(version).map_err(|error| {
                format!(
                    "{} --version printed '{}': {error}",
                    compiler.display(),
                    text.trim()
                )
            })
        })
}

/// The VM: `--marmotvm`, then MARMOTVM, the matching checkout preset,
/// an installed sibling executable, then marmotvm on PATH.
pub(crate) fn find_vm(explicit: Option<&Path>, compiler: &Path) -> PathBuf {
    if let Some(path) = explicit {
        return path.to_path_buf();
    }
    if let Some(path) = std::env::var_os("MARMOTVM").filter(|value| !value.is_empty()) {
        return PathBuf::from(path);
    }

    let name = format!("marmotvm{}", std::env::consts::EXE_SUFFIX);
    let checkout_vm = compiler.parent().and_then(|out| {
        let preset = out.parent()?;
        let project = preset.parent()?;
        if project.file_name()? != "marmotc" {
            return None;
        }
        Some(
            project
                .parent()?
                .join("marmotvm")
                .join(preset.file_name()?)
                .join("out")
                .join(&name),
        )
    });
    let beside_compiler = compiler.parent().map(|directory| directory.join(&name));
    let beside_tool = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|directory| directory.join(&name)));
    checkout_vm
        .into_iter()
        .chain(beside_compiler)
        .chain(beside_tool)
        .find(|candidate| candidate.is_file())
        .unwrap_or_else(|| PathBuf::from("marmotvm"))
}

pub(crate) fn run_compiler(mut command: Command, compiler: &Path) -> Result<ExitCode, String> {
    let status = command
        .status()
        .map_err(|error| format!("cannot run {}: {error}", compiler.display()))?;
    Ok(status
        .code()
        .map(|code| ExitCode::from((code & 0xFF) as u8))
        .unwrap_or(ExitCode::FAILURE))
}

#[cfg(test)]
mod tests;
