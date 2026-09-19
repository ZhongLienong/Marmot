use crate::paths;
use crate::version::{Constraint, Version};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use toml::{Table, Value};

pub const PROJECT_MANIFEST: &str = "project.marmot";
pub const PACKAGE_MANIFEST: &str = "package.marmot";

/// The FFI ABI the runtime implements; a package built for another one does
/// not load (`MarmotBuiltins::ABI_VERSION`).
const FFI_ABI_VERSION: i64 = 1;

/// The active manifest for a source file: the nearest `project.marmot` above
/// it, or a `package.marmot` with a `[package]` table.
#[derive(Debug, Clone)]
pub struct Workspace {
    pub root: PathBuf,
    pub manifest_path: PathBuf,
    pub entry: Option<PathBuf>,
    pub source_dir: PathBuf,
    pub packages_dir: PathBuf,
    pub prelude_dir: PathBuf,
    pub extra_paths: Vec<PathBuf>,
    pub dependencies: BTreeMap<String, Constraint>,
    /// `[test].dir`, relative to the root.
    pub test_dir: PathBuf,
    /// `[test].timeout_ms`, per test.
    pub test_timeout_ms: i64,
}

impl Workspace {
    pub fn entry_path(&self) -> Option<PathBuf> {
        self.entry
            .as_ref()
            .map(|entry| paths::under(&self.root, entry))
    }

    pub fn packages_path(&self) -> PathBuf {
        paths::under(&self.root, &self.packages_dir)
    }

    pub fn test_path(&self) -> PathBuf {
        paths::under(&self.root, &self.test_dir)
    }
}

pub fn read_toml(path: &Path) -> Result<Table, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
    text.parse::<Table>()
        .map_err(|error| format!("cannot parse {}: {error}", path.display()))
}

fn string_at<'a>(table: &'a Table, key: &str) -> Option<&'a str> {
    table
        .get(key)
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty())
}

fn directory_or(table: &Table, key: &str, fallback: &str) -> PathBuf {
    PathBuf::from(string_at(table, key).unwrap_or(fallback))
}

fn dependencies(data: &Table, context: &Path) -> Result<BTreeMap<String, Constraint>, String> {
    let Some(value) = data.get("dependencies") else {
        return Ok(BTreeMap::new());
    };
    let table = value
        .as_table()
        .ok_or_else(|| format!("{}: [dependencies] is not a table", context.display()))?;
    table
        .iter()
        .map(|(name, constraint)| {
            let text = constraint.as_str().ok_or_else(|| {
                format!("{}: dependency '{name}' is not a string", context.display())
            })?;
            Constraint::parse(text)
                .map(|constraint| (name.clone(), constraint))
                .map_err(|error| {
                    format!(
                        "Invalid dependency constraint for '{name}' in {}: {error}",
                        context.display()
                    )
                })
        })
        .collect()
}

fn test_settings(data: &Table) -> (PathBuf, i64) {
    let test = data.get("test").and_then(Value::as_table);
    (
        PathBuf::from(
            test.and_then(|test| string_at(test, "dir"))
                .unwrap_or("test"),
        ),
        test.and_then(|test| test.get("timeout_ms"))
            .and_then(Value::as_integer)
            .unwrap_or(30_000),
    )
}

fn load_workspace(manifest_path: &Path, root: &Path) -> Result<Option<Workspace>, String> {
    let data = read_toml(manifest_path)?;
    let (test_dir, test_timeout_ms) = test_settings(&data);

    if let Some(project) = data.get("project").and_then(Value::as_table) {
        let extra_paths = project
            .get("marmot_path")
            .and_then(Value::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(Value::as_str)
                    .map(PathBuf::from)
                    .collect()
            })
            .unwrap_or_default();
        return Ok(Some(Workspace {
            root: root.to_path_buf(),
            manifest_path: manifest_path.to_path_buf(),
            entry: string_at(project, "entry").map(PathBuf::from),
            source_dir: directory_or(project, "source_dir", "src"),
            packages_dir: directory_or(project, "packages_dir", "packages"),
            prelude_dir: directory_or(project, "prelude_dir", "MarmotPrelude"),
            extra_paths,
            dependencies: dependencies(&data, manifest_path)?,
            test_dir: test_dir.clone(),
            test_timeout_ms,
        }));
    }

    if manifest_path
        .file_name()
        .is_some_and(|name| name == PACKAGE_MANIFEST)
    {
        let Some(package) = data.get("package").and_then(Value::as_table) else {
            return Ok(None);
        };
        let entry = package
            .get("modules")
            .and_then(Value::as_table)
            .and_then(|modules| string_at(modules, "main"))
            .map(PathBuf::from);
        return Ok(Some(Workspace {
            root: root.to_path_buf(),
            manifest_path: manifest_path.to_path_buf(),
            entry,
            source_dir: PathBuf::from("."),
            packages_dir: PathBuf::from("packages"),
            prelude_dir: PathBuf::from("MarmotPrelude"),
            extra_paths: Vec::new(),
            dependencies: dependencies(&data, manifest_path)?,
            test_dir: test_dir.clone(),
            test_timeout_ms,
        }));
    }

    Err(format!(
        "{}: missing [project] table",
        manifest_path.display()
    ))
}

/// Walks up from `start` as the compiler's CLI does: in each directory a
/// `project.marmot` wins, then a `package.marmot` that has a `[package]`.
pub fn find_workspace(start: &Path) -> Result<Option<Workspace>, String> {
    for directory in start.ancestors() {
        let project = directory.join(PROJECT_MANIFEST);
        if project.exists() {
            return load_workspace(&project, directory);
        }

        let package = directory.join(PACKAGE_MANIFEST);
        if package.exists()
            && let Some(workspace) = load_workspace(&package, directory)?
        {
            return Ok(Some(workspace));
        }
    }

    Ok(None)
}

/// A `package.marmot` that loads: named, versioned, compatible with this
/// compiler and, if it has native code, with this runtime's FFI ABI.
#[derive(Debug, Clone)]
pub struct PackageManifest {
    pub directory: PathBuf,
    pub name: String,
    pub version: Version,
    pub version_text: String,
    pub dependencies: BTreeMap<String, Constraint>,
    pub native: Option<NativeLibrary>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NativeLibrary {
    pub library: PathBuf,
    pub functions: BTreeMap<String, String>,
    pub thread_safe: bool,
    pub checksum: Option<String>,
}

fn platform_prebuilt_key() -> &'static str {
    if cfg!(windows) {
        "windows_x64"
    } else if cfg!(all(target_os = "macos", target_arch = "aarch64")) {
        "macos_arm64"
    } else if cfg!(target_os = "macos") {
        "macos_x86_64"
    } else {
        "linux_x86_64"
    }
}

fn default_library_path(directory: &Path, library_name: &str) -> PathBuf {
    if cfg!(windows) {
        directory
            .join("lib")
            .join("windows")
            .join("x64")
            .join(format!("{library_name}.dll"))
    } else if cfg!(target_os = "macos") {
        directory
            .join("lib")
            .join("macos")
            .join(format!("lib{library_name}.dylib"))
    } else {
        directory
            .join("lib")
            .join("linux")
            .join("x86_64")
            .join(format!("lib{library_name}.so"))
    }
}

fn native_library(
    data: &Table,
    directory: &Path,
    name: &str,
) -> Result<Option<NativeLibrary>, String> {
    let Some(ffi) = data.get("ffi").and_then(Value::as_table) else {
        return Ok(None);
    };
    if !ffi.get("enabled").and_then(Value::as_bool).unwrap_or(false) {
        return Ok(None);
    }

    let abi_version = ffi
        .get("abi_version")
        .and_then(Value::as_integer)
        .unwrap_or(1);
    if abi_version <= 0 {
        return Err(format!(
            "Invalid [ffi].abi_version '{abi_version}'. Expected a positive integer."
        ));
    }
    if abi_version != FFI_ABI_VERSION {
        return Err(format!(
            "Package '{name}' targets FFI ABI v{abi_version}, but this Marmot runtime supports FFI ABI v{FFI_ABI_VERSION}."
        ));
    }

    let prebuilt = data
        .get("prebuilt")
        .and_then(Value::as_table)
        .and_then(|prebuilt| prebuilt.get(platform_prebuilt_key()))
        .and_then(Value::as_table);
    let prebuilt_path = prebuilt
        .and_then(|table| string_at(table, "path"))
        .map(|path| directory.join(path));

    // Without a library name the compiler finds no library, prebuilt or not.
    let Some(library_name) = string_at(ffi, "library_name") else {
        return Ok(None);
    };

    let mut functions = BTreeMap::new();
    if let Some(table) = ffi.get("functions").and_then(Value::as_table) {
        for (key, value) in table {
            let symbol = value
                .as_str()
                .ok_or_else(|| format!("[ffi.functions].{key} is not a string"))?;
            functions.insert(key.clone(), symbol.to_string());
        }
    }

    Ok(Some(NativeLibrary {
        library: prebuilt_path.unwrap_or_else(|| default_library_path(directory, library_name)),
        functions,
        thread_safe: ffi
            .get("thread_safe")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        checksum: prebuilt
            .and_then(|table| string_at(table, "checksum"))
            .map(str::to_string),
    }))
}

pub fn read_package(directory: &Path, compiler: &Version) -> Result<PackageManifest, String> {
    let manifest_path = directory.join(PACKAGE_MANIFEST);
    if !manifest_path.exists() {
        return Err(format!(
            "package.marmot not found in: {}",
            directory.display()
        ));
    }
    let data = read_toml(&manifest_path).map_err(|error| {
        format!(
            "Failed to parse package.marmot in {}: {error}",
            directory.display()
        )
    })?;
    let package = data
        .get("package")
        .and_then(Value::as_table)
        .ok_or_else(|| "Missing [package] table.".to_string())?;

    let name = string_at(package, "name").ok_or("Missing required package name.")?;
    let version_text = package.get("version").and_then(Value::as_str).unwrap_or("");
    let version = Version::parse(version_text)
        .map_err(|error| format!("Invalid package version '{version_text}': {error}"))?;

    let marmot_version = package
        .get("marmot_version")
        .and_then(Value::as_str)
        .unwrap_or(">=1.0.0");
    let required = Constraint::parse(marmot_version).map_err(|error| {
        format!("Invalid marmot_version constraint '{marmot_version}': {error}")
    })?;
    if !required.matches(compiler) {
        return Err(format!(
            "Package '{name}' requires Marmot {marmot_version}, but the current compiler version is {compiler}."
        ));
    }

    Ok(PackageManifest {
        directory: directory.to_path_buf(),
        name: name.to_string(),
        version,
        version_text: version_text.to_string(),
        dependencies: dependencies(&data, &manifest_path)?,
        native: native_library(&data, directory, name)?,
    })
}
