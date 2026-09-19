//! End-to-end runs of `marmot` against a real compiler, and against the
//! compiler's own package commands, which the tool must match. Set MARMOTC to
//! the compiler (marmotc) to run them; without it they are skipped.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicUsize, Ordering};

struct Project(PathBuf);

impl Drop for Project {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

impl Project {
    fn path(&self, relative: &str) -> PathBuf {
        self.0.join(relative)
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
/// registry, which in turn depends on a second package. The registry also
/// has an older Greeter and an unrelated package.
fn greeter_project(name: &str) -> Project {
    static COUNTER: AtomicUsize = AtomicUsize::new(0);
    let root = std::env::temp_dir().join(format!(
        "marmot-tool-e2e-{name}-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::SeqCst)
    ));
    let _ = std::fs::remove_dir_all(&root);
    write(
        &root,
        "project.marmot",
        "# The greeting app.\n[project]\nname = \"Greet\"\nentry = \"src/Main.mmt\"\nmarmot_path = [\"registry\"]\n\n[dependencies]\nGreeter = \"^1.0.0\"\n",
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
        "registry/Greeter/docs/notes.txt",
        "copied with the package, but not part of its checksum\n",
    );
    write(
        &root,
        "registry/GreeterOld/package.marmot",
        "[package]\nname = \"Greeter\"\nversion = \"1.0.5\"\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n",
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
    write(
        &root,
        "registry/Shout/package.marmot",
        "[package]\nname = \"Shout\"\nversion = \"0.3.1\"\n",
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

fn marmotc(compiler: &Path, directory: &Path, args: &[&str]) -> Output {
    Command::new(compiler)
        .args(args)
        .current_dir(directory)
        .env("MARMOT_PATH", prelude())
        .output()
        .unwrap()
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).replace("\r\n", "\n")
}

fn succeeded(output: &Output) -> &Output {
    assert!(
        output.status.success(),
        "{}{}",
        text(&output.stdout),
        text(&output.stderr)
    );
    output
}

/// A lockfile's packages, with everything the lockfile says about each; the
/// metadata (timestamp, writer) is left out.
fn locked_packages(project: &Project) -> BTreeMap<String, toml::Value> {
    let lock: toml::Table = std::fs::read_to_string(project.path("marmot.lock"))
        .unwrap()
        .parse()
        .unwrap();
    lock.get("package")
        .and_then(toml::Value::as_array)
        .map(|entries| {
            entries
                .iter()
                .map(|entry| (entry["name"].as_str().unwrap().to_string(), entry.clone()))
                .collect()
        })
        .unwrap_or_default()
}

fn manifest_checksum(project: &Project) -> String {
    let lock: toml::Table = std::fs::read_to_string(project.path("marmot.lock"))
        .unwrap()
        .parse()
        .unwrap();
    lock["metadata"]["manifest_checksum"]
        .as_str()
        .unwrap()
        .to_string()
}

fn files_under(root: &Path) -> BTreeMap<String, Vec<u8>> {
    fn walk(root: &Path, directory: &Path, files: &mut BTreeMap<String, Vec<u8>>) {
        let Ok(entries) = std::fs::read_dir(directory) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                walk(root, &path, files);
            } else {
                let relative = path
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                files.insert(relative, std::fs::read(&path).unwrap());
            }
        }
    }
    let mut files = BTreeMap::new();
    walk(root, root, &mut files);
    files
}

/// Runs the same command through the compiler and through the tool, each in
/// its own copy of the project, and checks both left the project the same.
fn both(
    compiler: &Path,
    name: &str,
    prepare: impl Fn(&Project),
    args: &[&str],
) -> (Project, Output, Project, Output) {
    let with_compiler = greeter_project(&format!("{name}-c"));
    let with_tool = greeter_project(&format!("{name}-t"));
    prepare(&with_compiler);
    prepare(&with_tool);
    let compiler_output = marmotc(compiler, &with_compiler.0, args);
    let tool_output = marmot(compiler, &with_tool.0, args);
    (with_compiler, compiler_output, with_tool, tool_output)
}

fn same_output(
    compiler_output: &Output,
    compiler_root: &Path,
    tool_output: &Output,
    tool_root: &Path,
) {
    let normalise = |output: &Output, root: &Path| {
        text(&output.stdout).replace(&root.display().to_string(), "<root>")
    };
    assert_eq!(
        normalise(tool_output, tool_root),
        normalise(compiler_output, compiler_root)
    );
}

/// The lockfile records the checksum of the manifest beside it. (The tool
/// edits manifests in place and the compiler rewrites them, so after an
/// edit the two manifests, and their checksums, differ in bytes.)
fn records_its_manifest(project: &Project) {
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(std::fs::read(project.path("project.marmot")).unwrap());
    let expected = format!(
        "sha256:{}",
        digest
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>()
    );
    assert_eq!(manifest_checksum(project), expected);
}

fn same_result(with_compiler: &Project, with_tool: &Project) {
    assert_eq!(locked_packages(with_tool), locked_packages(with_compiler));
    records_its_manifest(with_compiler);
    records_its_manifest(with_tool);
    assert_eq!(
        files_under(&with_tool.path("packages")),
        files_under(&with_compiler.path("packages"))
    );
    assert_eq!(
        toml::from_str::<toml::Table>(
            &std::fs::read_to_string(with_tool.path("project.marmot")).unwrap()
        )
        .unwrap(),
        toml::from_str::<toml::Table>(
            &std::fs::read_to_string(with_compiler.path("project.marmot")).unwrap()
        )
        .unwrap()
    );
}

#[test]
fn install_matches_the_compiler_and_each_reads_the_others_lockfile() {
    let Some(compiler) = compiler() else { return };
    let (with_compiler, compiler_output, with_tool, tool_output) =
        both(&compiler, "install", |_| {}, &["install"]);
    succeeded(&compiler_output);
    succeeded(&tool_output);
    same_output(
        &compiler_output,
        &with_compiler.0,
        &tool_output,
        &with_tool.0,
    );
    same_result(&with_compiler, &with_tool);
    assert!(
        with_tool
            .path("packages/Greeter-1.2.0/docs/notes.txt")
            .exists()
    );

    // Neither re-resolves from the other's lockfile: it stays byte for byte.
    let tool_lock = std::fs::read(with_tool.path("marmot.lock")).unwrap();
    succeeded(&marmotc(
        &compiler,
        &with_tool.0,
        &["check", "src/Main.mmt"],
    ));
    assert_eq!(
        std::fs::read(with_tool.path("marmot.lock")).unwrap(),
        tool_lock
    );

    let compiler_lock = std::fs::read(with_compiler.path("marmot.lock")).unwrap();
    succeeded(&marmot(&compiler, &with_compiler.0, &["check"]));
    assert_eq!(
        std::fs::read(with_compiler.path("marmot.lock")).unwrap(),
        compiler_lock
    );
}

#[test]
fn installing_a_new_dependency_matches_the_compiler() {
    let Some(compiler) = compiler() else { return };
    let (with_compiler, compiler_output, with_tool, tool_output) =
        both(&compiler, "add", |_| {}, &["install", "Shout"]);
    succeeded(&compiler_output);
    succeeded(&tool_output);
    same_output(
        &compiler_output,
        &with_compiler.0,
        &tool_output,
        &with_tool.0,
    );
    same_result(&with_compiler, &with_tool);
    assert!(text(&tool_output.stdout).contains("Added dependency Shout ^0.3.1"));
    // The tool edits the manifest in place; the compiler rewrites it.
    assert!(
        std::fs::read_to_string(with_tool.path("project.marmot"))
            .unwrap()
            .starts_with("# The greeting app.\n")
    );

    let (with_compiler, compiler_output, with_tool, tool_output) = both(
        &compiler,
        "add-version",
        |_| {},
        &["install", "Greeter", "--version", "~1.0.0"],
    );
    succeeded(&compiler_output);
    succeeded(&tool_output);
    same_output(
        &compiler_output,
        &with_compiler.0,
        &tool_output,
        &with_tool.0,
    );
    same_result(&with_compiler, &with_tool);
    assert!(locked_packages(&with_tool)["Greeter"]["version"].as_str() == Some("1.0.5"));
}

#[test]
fn update_list_and_remove_match_the_compiler() {
    let Some(compiler) = compiler() else { return };
    let installed = |project: &Project| {
        succeeded(&marmotc(&compiler_path(), &project.0, &["install"]));
    };
    fn compiler_path() -> PathBuf {
        std::env::var_os("MARMOTC").map(PathBuf::from).unwrap()
    }

    let (with_compiler, compiler_output, with_tool, tool_output) =
        both(&compiler, "list", installed, &["list"]);
    succeeded(&compiler_output);
    succeeded(&tool_output);
    same_output(
        &compiler_output,
        &with_compiler.0,
        &tool_output,
        &with_tool.0,
    );
    assert_eq!(
        text(&tool_output.stdout),
        "Greeter@1.2.0\n  Punctuation@1.0.0\n"
    );

    let newer_greeter = |project: &Project| {
        installed(project);
        write(
            &project.0,
            "registry/GreeterNew/package.marmot",
            "[package]\nname = \"Greeter\"\nversion = \"1.3.0\"\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n",
        );
    };
    let (with_compiler, compiler_output, with_tool, tool_output) =
        both(&compiler, "update", newer_greeter, &["update"]);
    succeeded(&compiler_output);
    succeeded(&tool_output);
    same_output(
        &compiler_output,
        &with_compiler.0,
        &tool_output,
        &with_tool.0,
    );
    same_result(&with_compiler, &with_tool);
    assert_eq!(
        text(&tool_output.stdout),
        "Updated packages:\nGreeter: 1.2.0 -> 1.3.0\n"
    );

    let (with_compiler, compiler_output, with_tool, tool_output) =
        both(&compiler, "remove", installed, &["remove", "Greeter"]);
    succeeded(&compiler_output);
    succeeded(&tool_output);
    same_output(
        &compiler_output,
        &with_compiler.0,
        &tool_output,
        &with_tool.0,
    );
    assert_eq!(locked_packages(&with_tool), locked_packages(&with_compiler));
    assert_eq!(
        files_under(&with_tool.path("packages")),
        files_under(&with_compiler.path("packages"))
    );
    assert!(!with_tool.path("packages/Greeter-1.2.0").exists());

    let (_, compiler_output, _, tool_output) = both(
        &compiler,
        "not-direct",
        installed,
        &["update", "Punctuation"],
    );
    assert!(!compiler_output.status.success());
    assert!(!tool_output.status.success());
    assert!(
        text(&tool_output.stderr).contains("Package 'Punctuation' is not a direct dependency.")
    );
}

#[test]
fn run_check_and_build_through_a_plan_match_the_compiler_on_its_own() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("run");

    let ran = succeeded(&marmot(&compiler, &project.0, &["run"]))
        .stdout
        .clone();
    let direct = succeeded(&marmotc(&compiler, &project.0, &["run", "src/Main.mmt"]))
        .stdout
        .clone();
    assert_eq!(text(&ran), text(&direct));
    assert!(text(&ran).contains("hello, plan!"), "{}", text(&ran));

    let checked = marmot(
        &compiler,
        &project.0,
        &["check", "src/Main.mmt", "--format", "json"],
    );
    succeeded(&checked);
    let report: serde_json::Value = serde_json::from_slice(&checked.stdout).unwrap();
    assert_eq!(report["success"], serde_json::Value::Bool(true));

    succeeded(&marmot(&compiler, &project.0, &["build"]));
    assert!(project.path("src/Main.mmc").exists());
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
