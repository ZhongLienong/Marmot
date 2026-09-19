use crate::paths;
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
    pub prelude_dir: PathBuf,
    pub extra_paths: Vec<PathBuf>,
}

impl Workspace {
    pub fn entry_path(&self) -> Option<PathBuf> {
        self.entry
            .as_ref()
            .map(|entry| paths::under(&self.root, entry))
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

fn directory_or(table: Option<&Table>, key: &str, fallback: &str) -> PathBuf {
    PathBuf::from(
        table
            .and_then(|table| string_at(table, key))
            .unwrap_or(fallback),
    )
}

fn load_workspace(manifest_path: &Path, root: &Path) -> Result<Option<Workspace>, String> {
    let data = read_toml(manifest_path)?;

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
            source_dir: directory_or(Some(project), "source_dir", "src"),
            prelude_dir: directory_or(Some(project), "prelude_dir", "MarmotPrelude"),
            extra_paths,
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
            prelude_dir: PathBuf::from("MarmotPrelude"),
            extra_paths: Vec::new(),
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

/// The parts of a `package.marmot` a plan needs.
#[derive(Debug, Clone)]
pub struct PackageManifest {
    pub name: String,
    pub version: String,
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

pub fn read_package(directory: &Path) -> Result<PackageManifest, String> {
    let manifest_path = directory.join(PACKAGE_MANIFEST);
    let data = read_toml(&manifest_path)?;
    let package = data
        .get("package")
        .and_then(Value::as_table)
        .ok_or_else(|| format!("{}: missing [package] table", manifest_path.display()))?;
    let name = string_at(package, "name")
        .ok_or_else(|| format!("{}: missing package name", manifest_path.display()))?;
    let version = string_at(package, "version")
        .ok_or_else(|| format!("{}: missing package version", manifest_path.display()))?;

    let native = match data.get("ffi").and_then(Value::as_table) {
        Some(ffi) if ffi.get("enabled").and_then(Value::as_bool).unwrap_or(false) => {
            let abi_version = ffi
                .get("abi_version")
                .and_then(Value::as_integer)
                .unwrap_or(1);
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
            let library = string_at(ffi, "library_name").map(|library_name| {
                prebuilt_path.unwrap_or_else(|| default_library_path(directory, library_name))
            });

            library.map(|library| NativeLibrary {
                library,
                functions: ffi
                    .get("functions")
                    .and_then(Value::as_table)
                    .map(|functions| {
                        functions
                            .iter()
                            .filter_map(|(key, value)| {
                                value
                                    .as_str()
                                    .map(|symbol| (key.clone(), symbol.to_string()))
                            })
                            .collect()
                    })
                    .unwrap_or_default(),
                thread_safe: ffi
                    .get("thread_safe")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                checksum: prebuilt
                    .and_then(|table| string_at(table, "checksum"))
                    .map(str::to_string),
            })
        }
        _ => None,
    };

    Ok(PackageManifest {
        name: name.to_string(),
        version: version.to_string(),
        native,
    })
}
