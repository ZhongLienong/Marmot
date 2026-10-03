use super::checksum::package_sources;
use super::lockfile;
use super::manifest::{self, Workspace};
use super::registry::{Lookup, Registries};
use super::resolver::{self, Graph, Index};
use super::version::Version;
use crate::checksum;
use crate::paths::{self, DirectoryList};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Mode {
    /// Use the lockfile when it still matches the manifest and every locked
    /// package is present; otherwise resolve afresh.
    PreferLockfile,
    /// Always resolve afresh.
    ForceRefresh,
}

#[derive(Debug)]
pub(crate) struct Environment {
    pub(crate) graph: Graph,
    pub(crate) search_paths: Vec<PathBuf>,
    pub(crate) warnings: Vec<String>,
    pub(crate) lockfile_path: PathBuf,
}

/// Where packages are installed for all projects of this user.
pub(crate) fn global_cache_directory() -> PathBuf {
    let variable = |name: &str| std::env::var_os(name).filter(|value| !value.is_empty());
    if cfg!(windows)
        && let Some(local_app_data) = variable("LOCALAPPDATA")
    {
        return PathBuf::from(local_app_data).join("Marmot").join("cache");
    }
    if let Some(home) = variable("USERPROFILE").or_else(|| variable("HOME")) {
        return PathBuf::from(home).join(".marmot").join("cache");
    }
    std::env::current_dir()
        .unwrap_or_default()
        .join(".marmot")
        .join("cache")
}

/// Search paths for a project, in the compiler CLI's order: the source
/// directory, packages in dependency order, `marmot_path` entries, the
/// prelude directory, then MARMOT_PATH.
fn search_paths(workspace: &Workspace, graph: &Graph, environment: &[PathBuf]) -> Vec<PathBuf> {
    let mut directories = DirectoryList::default();
    directories.push(paths::under(&workspace.root, &workspace.source_dir));
    for directory in graph.search_paths() {
        directories.push(directory);
    }
    for extra in &workspace.extra_paths {
        directories.push(paths::under(&workspace.root, extra));
    }
    directories.push(paths::under(&workspace.root, &workspace.prelude_dir));
    for path in environment {
        directories.push(path.clone());
    }
    directories.into_vec()
}

/// Where resolution looks for packages: the project's packages directory, the
/// global cache, `marmot_path` entries, then MARMOT_PATH.
fn index_roots(workspace: &Workspace, environment: &[PathBuf]) -> Vec<PathBuf> {
    let mut directories = DirectoryList::default();
    directories.push(workspace.packages_path());
    directories.push(global_cache_directory());
    for extra in &workspace.extra_paths {
        directories.push(paths::under(&workspace.root, extra));
    }
    for path in environment {
        directories.push(path.clone());
    }
    directories.into_vec()
}

/// The local package index, joined by the workspace's registries. Refreshing
/// asks them about every package; otherwise only about packages not on disk.
fn index(
    workspace: &Workspace,
    mode: Mode,
    environment: &[PathBuf],
    compiler: &Version,
) -> Result<Index, String> {
    let lookup = match mode {
        Mode::ForceRefresh => Lookup::Always,
        Mode::PreferLockfile => Lookup::WhenMissing,
    };
    let registries = Registries::new(&workspace.registries, global_cache_directory(), lookup)?;
    Ok(Index::scan(&index_roots(workspace, environment), compiler)?.with_registries(registries))
}

fn copy_directory(source: &Path, target: &Path) -> Result<(), String> {
    std::fs::create_dir_all(target)
        .map_err(|error| format!("Failed to create '{}': {error}", target.display()))?;
    let entries = std::fs::read_dir(source)
        .map_err(|error| format!("Failed to read '{}': {error}", source.display()))?;
    for entry in entries {
        let entry =
            entry.map_err(|error| format!("Failed to read '{}': {error}", source.display()))?;
        let from = entry.path();
        let to = target.join(entry.file_name());
        if entry.file_type().is_ok_and(|kind| kind.is_dir()) {
            copy_directory(&from, &to)?;
        } else {
            std::fs::copy(&from, &to).map_err(|error| {
                format!(
                    "Failed to copy package from '{}' to '{}': {error}",
                    from.display(),
                    to.display()
                )
            })?;
        }
    }
    Ok(())
}

fn copy_directory_replacing(source: &Path, target: &Path) -> Result<(), String> {
    if target.exists() {
        std::fs::remove_dir_all(target).map_err(|error| {
            format!(
                "Failed to clear package directory '{}': {error}",
                target.display()
            )
        })?;
    }
    copy_directory(source, target)
}

/// Copies each resolved package into `packages/<Name>-<version>` unless an
/// identical copy is already there, and points the graph at the copies.
fn install_locally(
    workspace: &Workspace,
    graph: &Graph,
    compiler: &Version,
) -> Result<Graph, String> {
    let packages_directory = workspace.packages_path();
    std::fs::create_dir_all(&packages_directory).map_err(|error| {
        format!(
            "Failed to create packages directory '{}': {error}",
            packages_directory.display()
        )
    })?;

    let mut localized = Graph {
        roots: graph.roots.clone(),
        ..Graph::default()
    };
    for name in graph.topological_order() {
        let package = &graph.packages[name];
        let source_directory = &package.manifest.directory;
        let target_directory =
            packages_directory.join(format!("{name}-{}", package.manifest.version_text));
        let source_checksum = if package.checksum.is_empty() {
            package_sources(source_directory)?
        } else {
            package.checksum.clone()
        };

        if paths::identity_key(source_directory) != paths::identity_key(&target_directory) {
            let current = target_directory
                .exists()
                .then(|| package_sources(&target_directory).ok())
                .flatten();
            if current.as_deref() != Some(source_checksum.as_str()) {
                copy_directory_replacing(source_directory, &target_directory)?;
            }
        }

        let mut localized_package = package.clone();
        localized_package.manifest = manifest::read_package(&target_directory, compiler)?;
        localized_package.checksum = source_checksum;
        localized_package.source = format!(
            "local:{}",
            paths::relative(&target_directory, &workspace.root)
        );
        localized
            .packages
            .insert(name.to_string(), localized_package);
    }
    Ok(localized)
}

/// Resolves the workspace's packages. Resolving afresh installs them into the
/// project and rewrites the lockfile, as the compiler's CLI always has.
pub(crate) fn prepare(
    workspace: &Workspace,
    mode: Mode,
    environment: &[PathBuf],
    compiler: &Version,
) -> Result<Environment, String> {
    let lockfile_path = workspace.root.join(lockfile::LOCKFILE);
    let manifest_checksum = checksum::file(&workspace.manifest_path)?;

    let mut warnings = Vec::new();
    if mode == Mode::PreferLockfile
        && let Some(lock) = lockfile::read(&workspace.root, compiler)
    {
        warnings = lock.warnings.clone();
        if lock.usable_for(&manifest_checksum) {
            return Ok(Environment {
                search_paths: search_paths(workspace, &lock.graph, environment),
                graph: lock.graph,
                warnings,
                lockfile_path,
            });
        }
    }

    let mut index = index(workspace, mode, environment, compiler)?;
    let resolved = resolver::resolve(&mut index, &workspace.dependencies)?;
    let localized = install_locally(workspace, &resolved, compiler)?;
    lockfile::write(&workspace.root, &localized, &manifest_checksum, compiler)?;

    Ok(Environment {
        search_paths: search_paths(workspace, &localized, environment),
        graph: localized,
        warnings,
        lockfile_path,
    })
}

/// The newest version of `name` any index root or registry offers.
pub(crate) fn newest_version(
    workspace: &Workspace,
    name: &str,
    environment: &[PathBuf],
    compiler: &Version,
) -> Result<Version, String> {
    index(workspace, Mode::ForceRefresh, environment, compiler)?
        .newest(name)?
        .ok_or_else(|| {
            format!("Package '{name}' was not found in the local package index or a registry.")
        })
}

/// Deletes installed copies the graph no longer uses. With `only`, just the
/// copies of that package.
pub(crate) fn remove_unused(
    workspace: &Workspace,
    graph: &Graph,
    only: Option<&str>,
    compiler: &Version,
) -> Result<(), String> {
    let packages_directory = workspace.packages_path();
    let Ok(entries) = std::fs::read_dir(&packages_directory) else {
        return Ok(());
    };
    for entry in entries.flatten() {
        let directory = entry.path();
        if !directory.is_dir() {
            continue;
        }
        let Ok(package) = manifest::read_package(&directory, compiler) else {
            continue;
        };
        if only.is_some_and(|name| name != package.name) {
            continue;
        }
        let active = graph
            .packages
            .get(&package.name)
            .is_some_and(|active| active.manifest.version_text == package.version_text);
        if !active {
            std::fs::remove_dir_all(&directory).map_err(|error| {
                format!(
                    "Failed to remove package directory '{}': {error}",
                    directory.display()
                )
            })?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;
