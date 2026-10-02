use std::path::{Path, PathBuf};

pub(crate) struct TemporaryFile(PathBuf);

impl TemporaryFile {
    pub(crate) fn new(path: PathBuf) -> TemporaryFile {
        TemporaryFile(path)
    }

    pub(crate) fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TemporaryFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// A directory removed when dropped.
pub(crate) struct TemporaryDirectory(PathBuf);

impl TemporaryDirectory {
    pub(crate) fn new(path: PathBuf) -> TemporaryDirectory {
        TemporaryDirectory(path)
    }
}

impl Drop for TemporaryDirectory {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
