use crate::manifest;
use sha2::{Digest, Sha256};
use std::cmp::Reverse;
use std::collections::{BTreeMap, BinaryHeap};
use std::path::{Path, PathBuf};
use toml::Value;

pub const LOCKFILE: &str = "marmot.lock";

#[derive(Debug, Clone)]
pub struct LockedPackage {
    pub name: String,
    pub directory: PathBuf,
    pub dependencies: Vec<String>,
}

#[derive(Debug)]
pub enum Lock {
    /// Every locked package is present and the manifest has not changed since
    /// the lockfile was written.
    Usable(Vec<LockedPackage>),
    /// The lockfile cannot be used as it is; the reason says why.
    Stale(String),
}

/// `sha256:<hex>` of the file's bytes, as the compiler records it.
pub fn file_checksum(path: &Path) -> Result<String, String> {
    let bytes =
        std::fs::read(path).map_err(|error| format!("cannot read {}: {error}", path.display()))?;
    let digest = Sha256::digest(&bytes);
    Ok(format!(
        "sha256:{}",
        digest
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>()
    ))
}

fn locked_directory(source: &str, root: &Path) -> PathBuf {
    match source.strip_prefix("local:") {
        Some(relative) => root.join(relative),
        None => PathBuf::from(source),
    }
}

pub fn read(root: &Path, manifest_path: &Path) -> Result<Lock, String> {
    let lockfile_path = root.join(LOCKFILE);
    if !lockfile_path.exists() {
        return Ok(Lock::Stale(format!("no {LOCKFILE}")));
    }

    let document = match manifest::read_toml(&lockfile_path) {
        Ok(document) => document,
        Err(error) => return Ok(Lock::Stale(error)),
    };
    let recorded = document
        .get("metadata")
        .and_then(Value::as_table)
        .and_then(|metadata| metadata.get("manifest_checksum"))
        .and_then(Value::as_str)
        .unwrap_or("");
    if recorded != file_checksum(manifest_path)? {
        return Ok(Lock::Stale(format!(
            "{} changed since {LOCKFILE} was written",
            manifest_path.display()
        )));
    }

    let mut packages = Vec::new();
    for entry in document
        .get("package")
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .unwrap_or_default()
    {
        let field = |key: &str| {
            entry
                .get(key)
                .and_then(Value::as_str)
                .filter(|value| !value.is_empty())
        };
        let (Some(name), Some(version), Some(source)) =
            (field("name"), field("version"), field("source"))
        else {
            return Ok(Lock::Stale(format!(
                "Malformed lockfile entry in {}",
                lockfile_path.display()
            )));
        };

        let directory = locked_directory(source, root);
        if !directory.exists() {
            return Ok(Lock::Stale(format!(
                "Locked package path is missing: {}",
                directory.display()
            )));
        }

        let package = match manifest::read_package(&directory) {
            Ok(package) => package,
            Err(error) => return Ok(Lock::Stale(error)),
        };
        if package.name != name || package.version != version {
            return Ok(Lock::Stale(format!(
                "Locked package '{name}' expected version {version}, but found {} at {}.",
                package.version,
                directory.display()
            )));
        }

        let dependencies = entry
            .get("dependencies")
            .and_then(Value::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(Value::as_str)
                    .map(|dependency| {
                        dependency
                            .split('@')
                            .next()
                            .unwrap_or(dependency)
                            .to_string()
                    })
                    .collect()
            })
            .unwrap_or_default();

        packages.push(LockedPackage {
            name: name.to_string(),
            directory,
            dependencies,
        });
    }

    Ok(Lock::Usable(packages))
}

/// Dependencies before dependents; among packages that are ready at the same
/// time, the alphabetically first goes first. Packages caught in a cycle are
/// left out, exactly as the compiler's own resolver does.
pub fn topological_order(packages: &[LockedPackage]) -> Vec<&LockedPackage> {
    let by_name: BTreeMap<&str, &LockedPackage> = packages
        .iter()
        .map(|package| (package.name.as_str(), package))
        .collect();
    let mut in_degree: BTreeMap<&str, usize> = by_name.keys().map(|name| (*name, 0)).collect();
    let mut dependents: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for package in by_name.values() {
        for dependency in &package.dependencies {
            if by_name.contains_key(dependency.as_str()) {
                dependents
                    .entry(dependency.as_str())
                    .or_default()
                    .push(package.name.as_str());
                *in_degree.entry(package.name.as_str()).or_default() += 1;
            }
        }
    }

    let mut ready: BinaryHeap<Reverse<&str>> = in_degree
        .iter()
        .filter(|(_, degree)| **degree == 0)
        .map(|(name, _)| Reverse(*name))
        .collect();
    let mut order = Vec::new();
    while let Some(Reverse(name)) = ready.pop() {
        order.push(by_name[name]);
        for dependent in dependents.get(name).map(Vec::as_slice).unwrap_or_default() {
            let degree = in_degree
                .get_mut(dependent)
                .expect("every dependent is a package");
            *degree -= 1;
            if *degree == 0 {
                ready.push(Reverse(dependent));
            }
        }
    }

    order
}
