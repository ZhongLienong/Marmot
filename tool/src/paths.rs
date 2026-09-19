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

/// The path's components joined with `/`.
pub fn generic(path: &Path) -> String {
    let mut text = String::new();
    for component in path.components() {
        match component {
            std::path::Component::Prefix(prefix) => {
                text.push_str(&prefix.as_os_str().to_string_lossy())
            }
            std::path::Component::RootDir => text.push('/'),
            other => {
                if !text.is_empty() && !text.ends_with('/') {
                    text.push('/');
                }
                text.push_str(&other.as_os_str().to_string_lossy());
            }
        }
    }
    text
}

fn resolved(path: &Path) -> PathBuf {
    let canonical = std::fs::canonicalize(path).unwrap_or_else(|_| absolute(path));
    match canonical.to_string_lossy().strip_prefix(r"\\?\") {
        Some(stripped) => PathBuf::from(stripped),
        None => canonical,
    }
}

/// `path` relative to `base`, climbing with `..` where needed, with `/`
/// separators; both are resolved first, like `std::filesystem::relative`.
pub fn relative(path: &Path, base: &Path) -> String {
    let path = resolved(path);
    let base = resolved(base);
    let same = |left: &std::path::Component, right: &std::path::Component| {
        if cfg!(windows) {
            left.as_os_str()
                .to_string_lossy()
                .eq_ignore_ascii_case(&right.as_os_str().to_string_lossy())
        } else {
            left == right
        }
    };

    let path_components: Vec<_> = path.components().collect();
    let base_components: Vec<_> = base.components().collect();
    let shared = path_components
        .iter()
        .zip(&base_components)
        .take_while(|(left, right)| same(left, right))
        .count();
    if shared == 0 {
        return generic(&path);
    }

    let mut parts: Vec<String> = vec!["..".to_string(); base_components.len() - shared];
    parts.extend(
        path_components[shared..]
            .iter()
            .map(|component| component.as_os_str().to_string_lossy().into_owned()),
    );
    if parts.is_empty() {
        ".".to_string()
    } else {
        parts.join("/")
    }
}
