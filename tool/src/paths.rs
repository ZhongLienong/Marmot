use std::path::{Path, PathBuf};

pub fn separator() -> char {
    if cfg!(windows) { ';' } else { ':' }
}

pub fn split_search_paths(value: &str) -> Vec<PathBuf> {
    value
        .split(separator())
        .filter(|segment| !segment.is_empty())
        .map(PathBuf::from)
        .collect()
}

pub fn marmot_path() -> Vec<PathBuf> {
    std::env::var("MARMOT_PATH")
        .map(|value| split_search_paths(&value))
        .unwrap_or_default()
}

pub fn absolute(path: &Path) -> PathBuf {
    std::path::absolute(path).unwrap_or_else(|_| path.to_path_buf())
}

/// `root/path`, or `path` itself when it is absolute.
pub fn under(root: &Path, path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        root.join(path)
    }
}

/// Existing directories in insertion order, each at most once. Two spellings
/// of one directory (case on Windows, `..`, symlinks) count as the same one.
#[derive(Default)]
pub struct DirectoryList {
    directories: Vec<PathBuf>,
    keys: Vec<String>,
}

impl DirectoryList {
    pub fn push(&mut self, candidate: PathBuf) {
        if candidate.as_os_str().is_empty() || !candidate.is_dir() {
            return;
        }

        let key = identity_key(&candidate);
        if !self.keys.contains(&key) {
            self.keys.push(key);
            self.directories.push(candidate);
        }
    }

    pub fn into_vec(self) -> Vec<PathBuf> {
        self.directories
    }
}

pub fn identity_key(path: &Path) -> String {
    let resolved = std::fs::canonicalize(path).unwrap_or_else(|_| absolute(path));
    let text = resolved.to_string_lossy();
    let text = text.strip_prefix(r"\\?\").unwrap_or(&text);
    if cfg!(windows) {
        text.to_lowercase()
    } else {
        text.to_string()
    }
}

/// A path as a plan writes it: absolute, without the Windows verbatim prefix.
pub fn display(path: &Path) -> String {
    let text = absolute(path).to_string_lossy().into_owned();
    match text.strip_prefix(r"\\?\") {
        Some(stripped) => stripped.to_string(),
        None => text,
    }
}
