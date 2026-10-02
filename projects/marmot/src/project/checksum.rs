use super::manifest::PACKAGE_MANIFEST;
use crate::checksum::{read, render};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};

fn collect_sources(directory: &Path, files: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(directory) else {
        return;
    };
    for entry in entries.flatten() {
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        let path = entry.path();
        if kind.is_dir() {
            collect_sources(&path, files);
        } else if kind.is_file() && path.extension().is_some_and(|extension| extension == "mmt") {
            files.push(path);
        }
    }
}

/// The checksum a lockfile records for a package: its manifest and every
/// `.mmt` file under it, in path order, each hashed as its path relative to
/// the package (with `/`), a newline, its bytes and a newline. Other files,
/// such as native libraries, do not count.
pub(super) fn package_sources(directory: &Path) -> Result<String, String> {
    let mut files = Vec::new();
    let manifest = directory.join(PACKAGE_MANIFEST);
    if manifest.exists() {
        files.push(manifest);
    }
    collect_sources(directory, &mut files);
    files.sort();

    let mut hash = Sha256::new();
    for file in &files {
        let relative = file.strip_prefix(directory).unwrap_or(file);
        let text = relative
            .components()
            .map(|component| component.as_os_str().to_string_lossy())
            .collect::<Vec<_>>()
            .join("/");
        hash.update(text.as_bytes());
        hash.update(b"\n");
        hash.update(read(file)?);
        hash.update(b"\n");
    }
    Ok(render(hash))
}
