use crate::manifest::{self, PACKAGE_MANIFEST};
use crate::packages::{self, Mode};
use crate::paths::{self, DirectoryList};
use crate::version::Version;
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

/// Native packages a program compiled from these directories can reach: the
/// entry's own package and any package that is itself a search path. The
/// compiler associates a file with the package whose manifest sits in the
/// file's directory, and a `<Name>` import finds `Name.mmt` directly in a
/// search path.
fn native_packages(
    entry_directory: &Path,
    search_paths: &[PathBuf],
    compiler: &Version,
) -> Result<Vec<PlanNativePackage>, String> {
    let mut packages: Vec<PlanNativePackage> = Vec::new();
    let mut seen_roots: Vec<String> = Vec::new();
    for directory in
        std::iter::once(entry_directory).chain(search_paths.iter().map(PathBuf::as_path))
    {
        let key = paths::identity_key(directory);
        if seen_roots.contains(&key) || !directory.join(PACKAGE_MANIFEST).exists() {
            continue;
        }
        seen_roots.push(key);

        // A manifest that does not load is not a native package, as in the compiler.
        let Ok(package) = manifest::read_package(directory, compiler) else {
            continue;
        };
        let Some(native) = package.native else {
            continue;
        };
        if let Some(existing) = packages
            .iter()
            .find(|existing| existing.name == package.name)
        {
            return Err(format!(
                "two native packages are named '{}': {} and {}",
                package.name,
                existing.root,
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

/// The plan for compiling `entry`, and any warnings about the project's
/// packages. Inside a project this resolves and installs packages when the
/// lockfile is missing or out of date.
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

    let (search_paths, warnings) = match manifest::find_workspace(&entry_directory)? {
        Some(workspace) => {
            let prepared =
                packages::prepare(&workspace, Mode::PreferLockfile, environment, compiler)?;
            (prepared.search_paths, prepared.warnings)
        }
        None => {
            let mut directories = DirectoryList::default();
            for path in environment {
                directories.push(path.clone());
            }
            (directories.into_vec(), Vec::new())
        }
    };

    let plan = Plan {
        version: PLAN_VERSION,
        entry: paths::display(&entry),
        native_packages: native_packages(&entry_directory, &search_paths, compiler)?,
        search_paths: search_paths
            .iter()
            .map(|path| paths::display(path))
            .collect(),
    };
    Ok((plan, warnings))
}
