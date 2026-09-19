use crate::lockfile::{self, Lock};
use crate::manifest::{self, Workspace};
use crate::paths::{self, DirectoryList};
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const PLAN_VERSION: u32 = 1;

/// The JSON document `marmotc --plan` reads.
#[derive(Debug, Serialize, PartialEq, Eq)]
pub struct Plan {
    pub version: u32,
    pub entry: String,
    pub search_paths: Vec<String>,
    pub native_packages: Vec<PlanNativePackage>,
}

#[derive(Debug, Serialize, PartialEq, Eq)]
pub struct PlanNativePackage {
    pub name: String,
    pub root: String,
    pub library: String,
    pub functions: BTreeMap<String, String>,
    pub thread_safe: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub checksum: Option<String>,
}

impl Plan {
    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).expect("a plan always serialises") + "\n"
    }
}

/// Brings a stale lockfile up to date. Until the package manager moves into
/// this tool, that is the compiler's `install`.
pub trait Resolver {
    fn resolve(&self, workspace: &Workspace, reason: &str) -> Result<(), String>;
}

/// Search paths for a project, in the compiler CLI's order: the source
/// directory, locked packages in dependency order, `marmot_path` entries, the
/// prelude directory, then MARMOT_PATH.
fn workspace_search_paths(
    workspace: &Workspace,
    environment: &[PathBuf],
    resolver: &dyn Resolver,
) -> Result<Vec<PathBuf>, String> {
    let packages = match lockfile::read(&workspace.root, &workspace.manifest_path)? {
        Lock::Usable(packages) => packages,
        Lock::Stale(reason) => {
            resolver.resolve(workspace, &reason)?;
            match lockfile::read(&workspace.root, &workspace.manifest_path)? {
                Lock::Usable(packages) => packages,
                Lock::Stale(reason) => {
                    return Err(format!(
                        "{} is still unusable after resolving: {reason}",
                        lockfile::LOCKFILE
                    ));
                }
            }
        }
    };

    let mut directories = DirectoryList::default();
    directories.push(paths::under(&workspace.root, &workspace.source_dir));
    for package in lockfile::topological_order(&packages) {
        directories.push(package.directory.clone());
    }
    for extra in &workspace.extra_paths {
        directories.push(paths::under(&workspace.root, extra));
    }
    directories.push(paths::under(&workspace.root, &workspace.prelude_dir));
    for path in environment {
        directories.push(path.clone());
    }
    Ok(directories.into_vec())
}

/// Native packages a program compiled from these directories can reach: the
/// entry's own package and any package that is itself a search path. The
/// compiler associates a file with the package whose manifest sits in the
/// file's directory, and a `<Name>` import finds `Name.mmt` directly in a
/// search path.
fn native_packages(
    entry_directory: &Path,
    search_paths: &[PathBuf],
) -> Result<Vec<PlanNativePackage>, String> {
    let mut packages: Vec<PlanNativePackage> = Vec::new();
    let mut seen_roots: Vec<String> = Vec::new();
    for directory in
        std::iter::once(entry_directory).chain(search_paths.iter().map(PathBuf::as_path))
    {
        let key = paths::identity_key(directory);
        if seen_roots.contains(&key) || !directory.join(manifest::PACKAGE_MANIFEST).exists() {
            continue;
        }
        seen_roots.push(key);

        // A manifest that does not load is not a native package, as in the compiler.
        let Ok(package) = manifest::read_package(directory) else {
            continue;
        };
        let Some(native) = package.native else {
            continue;
        };
        if packages
            .iter()
            .any(|existing| existing.name == package.name)
        {
            return Err(format!(
                "two native packages are named '{}': {} and {}",
                package.name,
                packages
                    .iter()
                    .find(|existing| existing.name == package.name)
                    .map(|existing| existing.root.as_str())
                    .unwrap_or(""),
                paths::display(directory)
            ));
        }

        packages.push(PlanNativePackage {
            name: package.name,
            root: paths::display(directory),
            library: paths::display(&native.library),
            functions: native.functions,
            thread_safe: native.thread_safe,
            checksum: native.checksum,
        });
    }

    Ok(packages)
}

pub fn make_plan(
    entry: &Path,
    environment: &[PathBuf],
    resolver: &dyn Resolver,
) -> Result<Plan, String> {
    let entry = paths::absolute(entry);
    if !entry.is_file() {
        return Err(format!("no such file: {}", entry.display()));
    }
    let entry_directory = entry.parent().map(Path::to_path_buf).unwrap_or_default();

    let search_paths = match manifest::find_workspace(&entry_directory)? {
        Some(workspace) => workspace_search_paths(&workspace, environment, resolver)?,
        None => {
            let mut directories = DirectoryList::default();
            for path in environment {
                directories.push(path.clone());
            }
            directories.into_vec()
        }
    };

    Ok(Plan {
        version: PLAN_VERSION,
        entry: paths::display(&entry),
        native_packages: native_packages(&entry_directory, &search_paths)?,
        search_paths: search_paths
            .iter()
            .map(|path| paths::display(path))
            .collect(),
    })
}
