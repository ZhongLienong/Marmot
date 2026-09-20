//! Whether a built program is still the program its inputs describe.
//!
//! `marmot run` writes a stamp beside the `.mmc`: the plan it was built from,
//! the compiler that built it, and every file it was built from with that
//! file's hash (marmotc lists them with `--deps`). When they all still match,
//! the build is skipped and the stamp replays what the build printed.

use crate::checksum;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

const VERSION: u32 = 1;

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Stamp {
    pub version: u32,
    /// The plan's JSON, hashed.
    pub plan: String,
    /// The compiler that built the program: its path, size and write time.
    pub compiler: String,
    /// Every source file the program was built from, hashed.
    pub files: BTreeMap<String, String>,
    /// What the build printed, replayed when the build is skipped.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output: Option<String>,
    /// The build's report, for `--format json`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub report: Option<Value>,
}

pub fn stamp_path(program: &Path) -> PathBuf {
    let mut name = program.as_os_str().to_os_string();
    name.push(".stamp");
    PathBuf::from(name)
}

pub fn deps_path(program: &Path) -> PathBuf {
    let mut name = program.as_os_str().to_os_string();
    name.push(".deps");
    PathBuf::from(name)
}

/// The compiler as a build input: a rebuilt marmotc invalidates what it built.
pub fn compiler_identity(compiler: &Path) -> String {
    let described = std::fs::metadata(compiler).ok().map(|metadata| {
        let written = metadata
            .modified()
            .ok()
            .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|since| since.as_nanos())
            .unwrap_or_default();
        format!("{} {} {written}", compiler.display(), metadata.len())
    });
    described.unwrap_or_else(|| compiler.display().to_string())
}

/// The files marmotc listed with `--deps`.
pub fn read_deps(path: &Path) -> Result<Vec<PathBuf>, String> {
    let listing = std::fs::read_to_string(path)
        .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
    Ok(listing
        .lines()
        .map(str::trim_end)
        .filter(|line| !line.is_empty())
        .map(PathBuf::from)
        .collect())
}

pub fn stamp(
    plan_json: &str,
    compiler: &Path,
    files: &[PathBuf],
    output: Option<String>,
    report: Option<Value>,
) -> Result<Stamp, String> {
    let mut hashed = BTreeMap::new();
    for file in files {
        hashed.insert(file.to_string_lossy().into_owned(), checksum::file(file)?);
    }
    Ok(Stamp {
        version: VERSION,
        plan: checksum::text(plan_json),
        compiler: compiler_identity(compiler),
        files: hashed,
        output,
        report,
    })
}

pub fn write(program: &Path, stamp: &Stamp) -> Result<(), String> {
    let path = stamp_path(program);
    let text =
        serde_json::to_string(stamp).map_err(|error| format!("cannot write a stamp: {error}"))?;
    std::fs::write(&path, text).map_err(|error| format!("cannot write {}: {error}", path.display()))
}

fn read(program: &Path) -> Option<Stamp> {
    let text = std::fs::read_to_string(stamp_path(program)).ok()?;
    let stamp: Stamp = serde_json::from_str(&text).ok()?;
    (stamp.version == VERSION).then_some(stamp)
}

/// The stamp of a build that is still current: the program is there, and the
/// plan, the compiler and every file it was built from are unchanged.
pub fn fresh(program: &Path, plan_json: &str, compiler: &Path) -> Option<Stamp> {
    if !program.is_file() {
        return None;
    }
    let stamp = read(program)?;
    if stamp.plan != checksum::text(plan_json) || stamp.compiler != compiler_identity(compiler) {
        return None;
    }
    for (file, hash) in &stamp.files {
        if checksum::file(Path::new(file)).ok()? != *hash {
            return None;
        }
    }
    Some(stamp)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Tree(PathBuf);

    impl Drop for Tree {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    impl Tree {
        fn new(name: &str) -> Tree {
            let root =
                std::env::temp_dir().join(format!("marmot-cache-{}-{name}", std::process::id()));
            let _ = std::fs::remove_dir_all(&root);
            std::fs::create_dir_all(&root).unwrap();
            Tree(root)
        }

        fn write(&self, relative: &str, contents: &str) -> PathBuf {
            let path = self.0.join(relative);
            std::fs::write(&path, contents).unwrap();
            path
        }
    }

    fn stamped(tree: &Tree, program: &Path, source: &Path, plan: &str) {
        let stamp = stamp(plan, source, &[source.to_path_buf()], None, None).unwrap();
        write(program, &stamp).unwrap();
        let _ = tree;
    }

    #[test]
    fn a_build_is_fresh_until_one_of_its_inputs_changes() {
        let tree = Tree::new("fresh");
        let program = tree.write("Main.mmc", "program");
        let source = tree.write("Main.mmt", "module Main\n");
        stamped(&tree, &program, &source, "{\"plan\":1}");

        assert!(fresh(&program, "{\"plan\":1}", &source).is_some());
        // A different plan, a changed source, a missing program: all stale.
        assert!(fresh(&program, "{\"plan\":2}", &source).is_none());
        tree.write("Main.mmt", "module Main\n// edited\n");
        assert!(fresh(&program, "{\"plan\":1}", &source).is_none());

        stamped(&tree, &program, &source, "{\"plan\":1}");
        assert!(fresh(&program, "{\"plan\":1}", &source).is_some());
        std::fs::remove_file(&program).unwrap();
        assert!(fresh(&program, "{\"plan\":1}", &source).is_none());
    }

    #[test]
    fn a_stamp_replays_what_the_build_printed() {
        let tree = Tree::new("replay");
        let program = tree.write("Main.mmc", "program");
        let source = tree.write("Main.mmt", "module Main\n");
        let stamp = stamp(
            "{}",
            &source,
            &[source.clone()],
            Some("warning: unused\n".to_string()),
            Some(serde_json::json!({ "warnings": [1] })),
        )
        .unwrap();
        write(&program, &stamp).unwrap();

        let read_back = fresh(&program, "{}", &source).unwrap();
        assert_eq!(read_back.output.as_deref(), Some("warning: unused\n"));
        assert_eq!(
            read_back.report,
            Some(serde_json::json!({ "warnings": [1] }))
        );
    }

    #[test]
    fn a_missing_source_file_is_not_fresh() {
        let tree = Tree::new("missing");
        let program = tree.write("Main.mmc", "program");
        let source = tree.write("Gone.mmt", "module Gone\n");
        stamped(&tree, &program, &source, "{}");
        std::fs::remove_file(&source).unwrap();

        assert!(fresh(&program, "{}", &source).is_none());
    }

    #[test]
    fn the_stamp_and_deps_sit_beside_the_program() {
        let program = Path::new("target/src/Main.mmc");
        assert_eq!(stamp_path(program), Path::new("target/src/Main.mmc.stamp"));
        assert_eq!(deps_path(program), Path::new("target/src/Main.mmc.deps"));
    }
}
