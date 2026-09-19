use crate::checksum;
use crate::manifest;
use crate::resolver::{Graph, ResolvedPackage};
use crate::version::Version;
use std::path::{Path, PathBuf};
use toml::{Table, Value};

pub const LOCKFILE: &str = "marmot.lock";

/// What a lockfile says, as far as the packages it names can still be found.
#[derive(Debug, Default)]
pub struct LockRead {
    pub graph: Graph,
    pub manifest_checksum: String,
    /// Every locked package is present, loads, and has its locked version.
    pub complete: bool,
    pub warnings: Vec<String>,
}

impl LockRead {
    /// Usable as it is for a manifest with this checksum.
    pub fn usable_for(&self, manifest_checksum: &str) -> bool {
        self.complete && self.manifest_checksum == manifest_checksum
    }
}

fn locked_directory(source: &str, root: &Path) -> PathBuf {
    match source.strip_prefix("local:") {
        Some(relative) => root.join(relative),
        None => PathBuf::from(source),
    }
}

/// Reads `root/marmot.lock`. `None` when there is no lockfile or it cannot be
/// read as one (the compiler then resolves afresh too).
pub fn read(root: &Path, compiler: &Version) -> Option<LockRead> {
    let path = root.join(LOCKFILE);
    if !path.exists() {
        return None;
    }
    let document = manifest::read_toml(&path).ok()?;

    let mut result = LockRead {
        manifest_checksum: document
            .get("metadata")
            .and_then(Value::as_table)
            .and_then(|metadata| metadata.get("manifest_checksum"))
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string(),
        complete: true,
        ..LockRead::default()
    };

    let entries = document
        .get("package")
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .unwrap_or_default();
    for entry in entries {
        let field = |key: &str| {
            entry
                .get(key)
                .and_then(Value::as_str)
                .filter(|value| !value.is_empty())
        };
        let (Some(name), Some(version), Some(source)) =
            (field("name"), field("version"), field("source"))
        else {
            return None;
        };
        let recorded_checksum = field("checksum").unwrap_or("");

        let directory = locked_directory(source, root);
        if !directory.exists() {
            result.complete = false;
            result.warnings.push(format!(
                "Locked package path is missing: {}",
                directory.display()
            ));
            continue;
        }

        let package = match manifest::read_package(&directory, compiler) {
            Ok(package) => package,
            Err(error) => {
                result.complete = false;
                result.warnings.push(error);
                continue;
            }
        };
        if package.name != name || package.version_text != version {
            result.complete = false;
            result.warnings.push(format!(
                "Locked package '{name}' expected version {version}, but found {} at {}.",
                package.version_text,
                directory.display()
            ));
            continue;
        }

        if !recorded_checksum.is_empty() {
            match checksum::package_sources(&directory) {
                Err(error) => result.warnings.push(error),
                Ok(current) if current != recorded_checksum => result.warnings.push(format!(
                    "Package '{name}' has changed since the lockfile was generated."
                )),
                Ok(_) => {}
            }
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

        result.graph.packages.insert(
            name.to_string(),
            ResolvedPackage {
                manifest: package,
                dependencies,
                source: source.to_string(),
                checksum: recorded_checksum.to_string(),
            },
        );
    }

    // A lockfile does not record which packages the project asked for; the
    // compiler takes every package nothing else depends on.
    result.graph.roots = result
        .graph
        .packages
        .keys()
        .filter(|name| {
            !result
                .graph
                .packages
                .values()
                .any(|package| package.dependencies.contains(name))
        })
        .cloned()
        .collect();
    Some(result)
}

/// `YYYY-MM-DDTHH:MM:SSZ` for now, in UTC.
fn utc_timestamp() -> String {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs() as i64)
        .unwrap_or(0);
    let (days, remainder) = (seconds.div_euclid(86_400), seconds.rem_euclid(86_400));

    // Howard Hinnant's days-to-civil conversion.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let day_of_era = z.rem_euclid(146_097);
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 {
        month_index + 3
    } else {
        month_index - 9
    };
    let year = year_of_era + era * 400 + i64::from(month <= 2);

    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        remainder / 3_600,
        remainder % 3_600 / 60,
        remainder % 60
    )
}

/// Writes the lockfile for `graph`, packages in dependency order.
pub fn write(
    root: &Path,
    graph: &Graph,
    manifest_checksum: &str,
    compiler: &Version,
) -> Result<(), String> {
    let mut metadata = Table::new();
    metadata.insert("marmot_version".into(), Value::String(compiler.to_string()));
    metadata.insert("generated".into(), Value::String(utc_timestamp()));
    metadata.insert(
        "manifest_checksum".into(),
        Value::String(manifest_checksum.to_string()),
    );

    let mut packages = Vec::new();
    for name in graph.topological_order() {
        let package = &graph.packages[name];
        let checksum = if package.checksum.is_empty() {
            checksum::package_sources(&package.manifest.directory)?
        } else {
            package.checksum.clone()
        };
        let dependencies = package
            .dependencies
            .iter()
            .map(|dependency| {
                Value::String(format!(
                    "{dependency}@{}",
                    graph.packages[dependency].manifest.version_text
                ))
            })
            .collect();

        let mut entry = Table::new();
        entry.insert("name".into(), Value::String(name.to_string()));
        entry.insert(
            "version".into(),
            Value::String(package.manifest.version_text.clone()),
        );
        entry.insert("source".into(), Value::String(package.source.clone()));
        entry.insert("checksum".into(), Value::String(checksum));
        entry.insert("dependencies".into(), Value::Array(dependencies));
        packages.push(Value::Table(entry));
    }

    let mut document = Table::new();
    document.insert("metadata".into(), Value::Table(metadata));
    document.insert("package".into(), Value::Array(packages));

    let text = format!(
        "# Auto-generated by Marmot. Do not edit manually.\n{}",
        toml::to_string(&document).map_err(|error| format!("cannot write {LOCKFILE}: {error}"))?
    );
    let path = root.join(LOCKFILE);
    std::fs::write(&path, text)
        .map_err(|error| format!("Failed to write lockfile: {}: {error}", path.display()))
}

#[cfg(test)]
mod tests {
    #[test]
    fn timestamps_are_utc_iso_8601() {
        let stamp = super::utc_timestamp();
        assert_eq!(stamp.len(), 20, "{stamp}");
        assert!(stamp.starts_with("20") && stamp.ends_with('Z'), "{stamp}");
        assert_eq!(&stamp[10..11], "T");
    }
}
