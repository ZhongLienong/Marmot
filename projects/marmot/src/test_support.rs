use crate::project::version::Version;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

pub(crate) struct TempTree(pub(crate) PathBuf);

impl TempTree {
    pub(crate) fn new(files: &[(&str, &str)]) -> TempTree {
        static COUNTER: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "marmot-tool-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::SeqCst)
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let tree = TempTree(root);
        for (relative, contents) in files {
            tree.write(relative, contents);
        }
        tree
    }

    pub(crate) fn path(&self, relative: &str) -> PathBuf {
        self.0.join(relative)
    }

    pub(crate) fn write(&self, relative: &str, contents: &str) {
        let path = self.path(relative);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, contents).unwrap();
    }

    pub(crate) fn read(&self, relative: &str) -> String {
        std::fs::read_to_string(self.path(relative)).unwrap()
    }
}

impl Drop for TempTree {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

pub(crate) fn compiler() -> Version {
    Version::parse("1.0.0").unwrap()
}

pub(crate) fn package(name: &str, version: &str, dependencies: &[(&str, &str)]) -> String {
    let mut text = format!("[package]\nname = \"{name}\"\nversion = \"{version}\"\n");
    if !dependencies.is_empty() {
        text += "\n[dependencies]\n";
        for (dependency, constraint) in dependencies {
            text += &format!("{dependency} = \"{constraint}\"\n");
        }
    }
    text
}

pub(crate) fn project(dependencies: &[(&str, &str)]) -> String {
    let mut text =
        "[project]\nentry = \"src/Main.mmt\"\nmarmot_path = [\"registry\"]\n".to_string();
    if !dependencies.is_empty() {
        text += "\n[dependencies]\n";
        for (dependency, constraint) in dependencies {
            text += &format!("{dependency} = \"{constraint}\"\n");
        }
    }
    text
}
