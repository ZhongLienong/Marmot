use crate::manifest::{self, PACKAGE_MANIFEST};
use crate::packages::{self, Mode};
use crate::paths::{self, DirectoryList};
use crate::version::Version;
use serde::Serialize;
use std::path::{Path, PathBuf};

pub const PLAN_VERSION: u32 = 1;

/// The JSON document `marmotc --plan` reads.
#[derive(Debug, Serialize, PartialEq, Eq)]
pub struct Plan {
    pub version: u32,
    /// Absent in a plan for `marmotc test`, which compiles many files.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub entry: Option<String>,
    pub search_paths: Vec<String>,
    pub native_libraries: Vec<PlanNativeLibrary>,
}

/// How a library that source names with `from "name"` may be used, which the
/// compiler records in the program, and where the tool found its file.
#[derive(Debug, Serialize, PartialEq, Eq)]
pub struct PlanNativeLibrary {
    pub name: String,
    /// Not part of the plan: where a file is belongs to the run, which the tool
    /// passes to marmotvm.
    #[serde(skip)]
    pub path: String,
    pub thread_safe: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub checksum: Option<String>,
}

impl Plan {
    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).expect("a plan always serialises") + "\n"
    }
}

/// The native libraries of the packages a program compiled from these
/// directories can reach: the entry's own package and any package that is
/// itself a search path.
fn native_libraries(
    entry_directory: &Path,
    search_paths: &[PathBuf],
    compiler: &Version,
    warnings: &mut Vec<String>,
) -> Result<Vec<PlanNativeLibrary>, String> {
    let mut libraries: Vec<PlanNativeLibrary> = Vec::new();
    let mut providers: Vec<String> = Vec::new();
    let mut seen_roots: Vec<String> = Vec::new();
    for directory in
        std::iter::once(entry_directory).chain(search_paths.iter().map(PathBuf::as_path))
    {
        let key = paths::identity_key(directory);
        if seen_roots.contains(&key) || !directory.join(PACKAGE_MANIFEST).exists() {
            continue;
        }
        seen_roots.push(key);

        // A manifest that does not load provides no library; say why.
        let package = match manifest::read_package(directory, compiler) {
            Ok(package) => package,
            Err(error) => {
                warnings.push(format!(
                    "{}: {error}",
                    paths::display(&directory.join(PACKAGE_MANIFEST))
                ));
                continue;
            }
        };
        let Some(native) = package.native else {
            continue;
        };
        if let Some(index) = libraries
            .iter()
            .position(|existing| existing.name == native.name)
        {
            return Err(format!(
                "two packages provide the native library '{}': {} and {}",
                native.name,
                providers[index],
                paths::display(directory)
            ));
        }

        providers.push(paths::display(directory));
        libraries.push(PlanNativeLibrary {
            name: native.name,
            path: paths::display(&native.library),
            thread_safe: native.thread_safe,
            checksum: native.checksum,
        });
    }

    Ok(libraries)
}

/// Search paths for sources under `directory`, and any warnings about the
/// project's packages. Inside a project this resolves and installs packages
/// when the lockfile is missing or out of date.
fn inputs(
    directory: &Path,
    environment: &[PathBuf],
    compiler: &Version,
) -> Result<(Vec<PathBuf>, Vec<String>), String> {
    match manifest::find_workspace(directory)? {
        Some(workspace) => {
            let prepared =
                packages::prepare(&workspace, Mode::PreferLockfile, environment, compiler)?;
            Ok((prepared.search_paths, prepared.warnings))
        }
        None => {
            let mut directories = DirectoryList::default();
            for path in environment {
                directories.push(path.clone());
            }
            Ok((directories.into_vec(), Vec::new()))
        }
    }
}

fn assemble(
    entry: Option<&Path>,
    directory: &Path,
    search_paths: Vec<PathBuf>,
    compiler: &Version,
    warnings: &mut Vec<String>,
) -> Result<Plan, String> {
    Ok(Plan {
        version: PLAN_VERSION,
        entry: entry.map(paths::display),
        native_libraries: native_libraries(directory, &search_paths, compiler, warnings)?,
        search_paths: search_paths
            .iter()
            .map(|path| paths::display(path))
            .collect(),
    })
}

/// The plan for compiling `entry`, and any warnings about its project's packages.
pub fn make_plan(
    entry: &Path,
    environment: &[PathBuf],
    compiler: &Version,
) -> Result<(Plan, Vec<String>), String> {
    let entry = paths::absolute(entry);
    if !entry.is_file() {
        return Err(format!("no such file: {}", entry.display()));
    }
    let entry_directory = entry.parent().map(Path::to_path_buf).unwrap_or_default();
    let (search_paths, mut warnings) = inputs(&entry_directory, environment, compiler)?;
    let plan = assemble(
        Some(&entry),
        &entry_directory,
        search_paths,
        compiler,
        &mut warnings,
    )?;
    Ok((plan, warnings))
}

/// A plan with no entry, for compiling every file under `directory` (tests).
pub fn inputs_plan(
    directory: &Path,
    environment: &[PathBuf],
    compiler: &Version,
) -> Result<(Plan, Vec<String>), String> {
    let directory = paths::absolute(directory);
    let (search_paths, mut warnings) = inputs(&directory, environment, compiler)?;
    let plan = assemble(None, &directory, search_paths, compiler, &mut warnings)?;
    Ok((plan, warnings))
}
