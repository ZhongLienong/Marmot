//! End-to-end runs of `marmot` against a real compiler. Set MARMOTC to the
//! compiler (Marmot.exe) to run them; without it they are skipped.

use std::path::{Path, PathBuf};
use std::process::{Command, Output};

struct Project(PathBuf);

impl Drop for Project {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn write(root: &Path, relative: &str, contents: &str) {
    let path = root.join(relative);
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, contents).unwrap();
}

fn compiler() -> Option<PathBuf> {
    let compiler = std::env::var_os("MARMOTC")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from);
    if compiler.is_none() {
        eprintln!("skipped: set MARMOTC to the compiler to run the end-to-end tests");
    }
    compiler
}

fn prelude() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("MarmotPrelude")
}

/// A project that depends on a package found through its `marmot_path`
/// registry, which in turn depends on a second package.
fn greeter_project(name: &str) -> Project {
    let root = std::env::temp_dir().join(format!("marmot-tool-e2e-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    write(
        &root,
        "project.marmot",
        "[project]\nname = \"Greet\"\nentry = \"src/Main.mmt\"\nmarmot_path = [\"registry\"]\n\n[dependencies]\nGreeter = \"^1.0.0\"\n",
    );
    write(
        &root,
        "src/Main.mmt",
        "module Main\n\nimport\n{\n    \"<IO>\",\n    \"<Greeter>\",\n}\n\nIO::PrintLine(Greeter::Hello(\"plan\"));\n",
    );
    write(
        &root,
        "registry/Greeter/package.marmot",
        "[package]\nname = \"Greeter\"\nversion = \"1.2.0\"\n\n[package.modules]\nmain = \"Greeter.mmt\"\nexports = [\"Greeter\"]\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n",
    );
    write(
        &root,
        "registry/Greeter/Greeter.mmt",
        "module Greeter\npublic export { Hello }\n\nimport { \"<Punctuation>\" }\n\ndef Hello = fn(name: Text) -> Text => \"hello, \" ++ name ++ Punctuation::Mark();\n",
    );
    write(
        &root,
        "registry/Punctuation/package.marmot",
        "[package]\nname = \"Punctuation\"\nversion = \"1.0.0\"\n\n[package.modules]\nmain = \"Punctuation.mmt\"\nexports = [\"Punctuation\"]\n",
    );
    write(
        &root,
        "registry/Punctuation/Punctuation.mmt",
        "module Punctuation\npublic export { Mark }\n\ndef Mark = fn() -> Text => \"!\";\n",
    );
    Project(root)
}

fn marmot(compiler: &Path, directory: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_marmot"))
        .args(args)
        .current_dir(directory)
        .env("MARMOTC", compiler)
        .env("MARMOT_PATH", prelude())
        .output()
        .unwrap()
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).replace("\r\n", "\n")
}

#[test]
fn the_tool_resolves_once_then_reads_the_compilers_lockfile() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("lock");

    let first = marmot(&compiler, &project.0, &["plan"]);
    assert!(first.status.success(), "{}", text(&first.stderr));
    assert!(
        text(&first.stderr).contains("marmot: resolving packages (no marmot.lock)"),
        "{}",
        text(&first.stderr)
    );
    assert!(project.0.join("marmot.lock").exists());

    // The lockfile was written by the compiler's `install`. Reading it back
    // without resolving again means the manifest checksums agree.
    let second = marmot(&compiler, &project.0, &["plan"]);
    assert!(second.status.success(), "{}", text(&second.stderr));
    assert!(
        !text(&second.stderr).contains("resolving"),
        "{}",
        text(&second.stderr)
    );
    assert_eq!(text(&first.stdout), text(&second.stdout));

    let plan: serde_json::Value = serde_json::from_slice(&second.stdout).unwrap();
    let search_paths: Vec<String> = plan["search_paths"]
        .as_array()
        .unwrap()
        .iter()
        .map(|path| {
            Path::new(path.as_str().unwrap())
                .file_name()
                .unwrap()
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    assert_eq!(
        search_paths,
        vec![
            "src",
            "Punctuation-1.0.0",
            "Greeter-1.2.0",
            "registry",
            "MarmotPrelude"
        ]
    );
}

#[test]
fn run_check_and_build_through_a_plan_match_the_compiler_on_its_own() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("run");

    let ran = marmot(&compiler, &project.0, &["run"]);
    assert!(
        ran.status.success(),
        "{}{}",
        text(&ran.stdout),
        text(&ran.stderr)
    );

    let direct = Command::new(&compiler)
        .args(["run", "src/Main.mmt"])
        .current_dir(&project.0)
        .env("MARMOT_PATH", prelude())
        .output()
        .unwrap();
    assert!(
        direct.status.success(),
        "{}{}",
        text(&direct.stdout),
        text(&direct.stderr)
    );
    assert_eq!(text(&ran.stdout), text(&direct.stdout));
    assert!(
        text(&ran.stdout).contains("hello, plan!"),
        "{}",
        text(&ran.stdout)
    );

    let checked = marmot(
        &compiler,
        &project.0,
        &["check", "src/Main.mmt", "--format", "json"],
    );
    assert!(checked.status.success(), "{}", text(&checked.stdout));
    let report: serde_json::Value = serde_json::from_slice(&checked.stdout).unwrap();
    assert_eq!(report["success"], serde_json::Value::Bool(true));

    let built = marmot(&compiler, &project.0, &["build"]);
    assert!(
        built.status.success(),
        "{}{}",
        text(&built.stdout),
        text(&built.stderr)
    );
    assert!(project.0.join("src").join("Main.mmc").exists());
}

#[test]
fn a_compile_error_comes_back_with_the_compilers_exit_status() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("error");
    write(&project.0, "src/Main.mmt", "module Main\ndef broken = ;\n");

    let checked = marmot(&compiler, &project.0, &["check"]);
    assert!(!checked.status.success());
    assert!(
        text(&checked.stdout).contains("Parser Error"),
        "{}",
        text(&checked.stdout)
    );
}
