//! End-to-end runs of `marmot` against a real compiler. Set MARMOTC to the
//! compiler (marmotc) to run them; without it they are skipped.
//!
//! The package commands' expected results below (lockfile entries, vendored
//! trees, output) were first checked against the compiler's own package
//! manager on identical projects, before it was removed.

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
    fn new(name: &str) -> Project {
        static COUNTER: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "marmot-tool-e2e-{name}-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::SeqCst)
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        Project(root)
    }

    fn path(&self, relative: &str) -> PathBuf {
        self.0.join(relative)
    }

    fn write(&self, relative: &str, contents: &str) {
        let path = self.path(relative);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, contents).unwrap();
    }

    fn read(&self, relative: &str) -> String {
        std::fs::read_to_string(self.path(relative)).unwrap()
    }
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
    let project = Project::new(name);
    project.write(
        "project.marmot",
        "# The greeting app.\n[project]\nname = \"Greet\"\nentry = \"src/Main.mmt\"\nmarmot_path = [\"registry\"]\n\n[dependencies]\nGreeter = \"^1.0.0\"\n",
    );
    project.write(
        "src/Main.mmt",
        "module Main\n\nimport\n{\n    \"<IO>\",\n    \"<Greeter>\",\n}\n\nIO::PrintLine(Greeter::Hello(\"plan\"));\n",
    );
    project.write(
        "registry/Greeter/package.marmot",
        "[package]\nname = \"Greeter\"\nversion = \"1.2.0\"\n\n[package.modules]\nmain = \"Greeter.mmt\"\nexports = [\"Greeter\"]\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n",
    );
    project.write(
        "registry/Greeter/Greeter.mmt",
        "module Greeter\npublic export { Hello }\n\nimport { \"<Punctuation>\" }\n\ndef Hello = fn(name: Text) -> Text => \"hello, \" ++ name ++ Punctuation::Mark();\n",
    );
    project.write(
        "registry/Greeter/docs/notes.txt",
        "copied with the package, but not part of its checksum\n",
    );
    project.write(
        "registry/GreeterOld/package.marmot",
        "[package]\nname = \"Greeter\"\nversion = \"1.0.5\"\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n",
    );
    project.write(
        "registry/Punctuation/package.marmot",
        "[package]\nname = \"Punctuation\"\nversion = \"1.0.0\"\n\n[package.modules]\nmain = \"Punctuation.mmt\"\nexports = [\"Punctuation\"]\n",
    );
    project.write(
        "registry/Punctuation/Punctuation.mmt",
        "module Punctuation\npublic export { Mark }\n\ndef Mark = fn() -> Text => \"!\";\n",
    );
    project.write(
        "registry/Shout/package.marmot",
        "[package]\nname = \"Shout\"\nversion = \"0.3.1\"\n",
    );
    project
}

fn run(program: &Path, directory: &Path, args: &[&str], compiler: &Path) -> Output {
    Command::new(program)
        .args(args)
        .current_dir(directory)
        .env("MARMOTC", compiler)
        .env("MARMOT_PATH", prelude())
        .output()
        .unwrap()
}

fn marmot(compiler: &Path, directory: &Path, args: &[&str]) -> Output {
    run(
        Path::new(env!("CARGO_BIN_EXE_marmot")),
        directory,
        args,
        compiler,
    )
}

/// marmotvm, which the tool finds beside the compiler.
fn vm(compiler: &Path) -> PathBuf {
    compiler.with_file_name(format!("marmotvm{}", std::env::consts::EXE_SUFFIX))
}

fn marmotc(compiler: &Path, directory: &Path, args: &[&str]) -> Output {
    run(compiler, directory, args, compiler)
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

fn stdout(project: &Project, output: &Output) -> String {
    text(&output.stdout).replace(&project.0.display().to_string(), "<root>")
}

fn lockfile(project: &Project) -> toml::Table {
    project.read("marmot.lock").parse().unwrap()
}

/// A lockfile's packages by name: version, source and dependencies.
fn locked(project: &Project) -> BTreeMap<String, (String, String, Vec<String>)> {
    lockfile(project)
        .get("package")
        .and_then(toml::Value::as_array)
        .map(|entries| {
            entries
                .iter()
                .map(|entry| {
                    let field = |key: &str| entry[key].as_str().unwrap().to_string();
                    let dependencies = entry["dependencies"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .map(|value| value.as_str().unwrap().to_string())
                        .collect();
                    (
                        field("name"),
                        (field("version"), field("source"), dependencies),
                    )
                })
                .collect()
        })
        .unwrap_or_default()
}

fn sha256(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    format!(
        "sha256:{}",
        Sha256::digest(bytes)
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>()
    )
}

fn files_under(root: &Path) -> Vec<String> {
    fn walk(root: &Path, directory: &Path, files: &mut Vec<String>) {
        let Ok(entries) = std::fs::read_dir(directory) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                walk(root, &path, files);
            } else {
                files.push(
                    path.strip_prefix(root)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/"),
                );
            }
        }
    }
    let mut files = Vec::new();
    walk(root, root, &mut files);
    files.sort();
    files
}

#[test]
fn install_resolves_vendors_and_locks() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("install");

    let installed = marmot(&compiler, &project.0, &["install"]);
    succeeded(&installed);
    assert_eq!(
        stdout(&project, &installed),
        format!(
            "Installed 2 package(s); lockfile updated at <root>{}marmot.lock\nGreeter@1.2.0\n  Punctuation@1.0.0\n",
            std::path::MAIN_SEPARATOR
        )
    );
    assert_eq!(
        locked(&project),
        BTreeMap::from([
            (
                "Greeter".to_string(),
                (
                    "1.2.0".to_string(),
                    "local:packages/Greeter-1.2.0".to_string(),
                    vec!["Punctuation@1.0.0".to_string()]
                )
            ),
            (
                "Punctuation".to_string(),
                (
                    "1.0.0".to_string(),
                    "local:packages/Punctuation-1.0.0".to_string(),
                    Vec::new()
                )
            ),
        ])
    );
    assert_eq!(
        lockfile(&project)["metadata"]["manifest_checksum"].as_str(),
        Some(sha256(project.read("project.marmot").as_bytes()).as_str())
    );
    assert_eq!(
        files_under(&project.path("packages")),
        vec![
            "Greeter-1.2.0/Greeter.mmt",
            "Greeter-1.2.0/docs/notes.txt",
            "Greeter-1.2.0/package.marmot",
            "Punctuation-1.0.0/Punctuation.mmt",
            "Punctuation-1.0.0/package.marmot",
        ]
    );

    // A current lockfile is read, not rewritten.
    let lock = project.read("marmot.lock");
    succeeded(&marmot(&compiler, &project.0, &["check"]));
    assert_eq!(project.read("marmot.lock"), lock);
}

#[test]
fn installing_a_dependency_records_it_and_keeps_the_manifest() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("add");

    let added = marmot(&compiler, &project.0, &["install", "Shout"]);
    succeeded(&added);
    assert!(stdout(&project, &added).contains("Added dependency Shout ^0.3.1\n"));
    let manifest = project.read("project.marmot");
    assert!(manifest.starts_with("# The greeting app.\n"), "{manifest}");
    assert!(manifest.contains("Shout = \"^0.3.1\""), "{manifest}");
    assert!(locked(&project).contains_key("Shout"));

    let pinned = greeter_project("pin");
    succeeded(&marmot(
        &compiler,
        &pinned.0,
        &["install", "Greeter", "--version", "~1.0.0"],
    ));
    assert_eq!(locked(&pinned)["Greeter"].0, "1.0.5");
}

#[test]
fn update_list_and_remove() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("manage");
    succeeded(&marmot(&compiler, &project.0, &["install"]));

    let listed = marmot(&compiler, &project.0, &["list"]);
    assert_eq!(
        stdout(&project, succeeded(&listed)),
        "Greeter@1.2.0\n  Punctuation@1.0.0\n"
    );

    project.write(
        "registry/GreeterNew/package.marmot",
        "[package]\nname = \"Greeter\"\nversion = \"1.3.0\"\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n",
    );
    let updated = marmot(&compiler, &project.0, &["update"]);
    assert_eq!(
        stdout(&project, succeeded(&updated)),
        "Updated packages:\nGreeter: 1.2.0 -> 1.3.0\n"
    );

    let not_direct = marmot(&compiler, &project.0, &["update", "Punctuation"]);
    assert!(!not_direct.status.success());
    assert!(text(&not_direct.stderr).contains("Package 'Punctuation' is not a direct dependency."));

    let removed = marmot(&compiler, &project.0, &["remove", "Greeter"]);
    assert_eq!(
        stdout(&project, succeeded(&removed)),
        "Removed dependency Greeter\n"
    );
    assert!(!project.path("packages/Greeter-1.3.0").exists());
    assert!(!locked(&project).contains_key("Greeter"));
}

#[test]
fn run_check_build_and_the_plan_handoff() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("run");

    // marmotc builds into target/, mirroring the entry's path, and marmotvm runs it.
    let ran = marmot(&compiler, &project.0, &["run"]);
    assert_eq!(text(&succeeded(&ran).stdout), "hello, plan!\n");
    assert!(project.path("target/src/Main.mmc").exists());
    assert!(!project.path("src/Main.mmc").exists());

    // The tool's plan is all the compiler needs, and the .mmc all the VM needs.
    succeeded(&marmot(&compiler, &project.0, &["plan", "-o", "plan.json"]));
    succeeded(&marmotc(
        &compiler,
        &project.0,
        &["build", "--plan", "plan.json", "-o", "direct.mmc"],
    ));
    let direct = run(&vm(&compiler), &project.0, &["direct.mmc"], &compiler);
    assert_eq!(text(&succeeded(&direct).stdout), "hello, plan!\n");

    // Without the plan, the compiler knows nothing of the project's packages.
    let bare = marmotc(&compiler, &project.0, &["check", "src/Main.mmt"]);
    assert!(!bare.status.success());
    assert!(text(&bare.stdout).contains("Could not resolve import: <Greeter>"));

    let checked = marmot(
        &compiler,
        &project.0,
        &["check", "src/Main.mmt", "--format", "json"],
    );
    let report: serde_json::Value = serde_json::from_slice(&succeeded(&checked).stdout).unwrap();
    assert_eq!(report["success"], serde_json::Value::Bool(true));

    succeeded(&marmot(&compiler, &project.0, &["build"]));
    assert!(project.path("src/Main.mmc").exists());
}

#[test]
fn a_compile_error_comes_back_with_the_compilers_exit_status() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("error");
    project.write("src/Main.mmt", "module Main\ndef broken = ;\n");

    let checked = marmot(&compiler, &project.0, &["check"]);
    assert!(!checked.status.success());
    assert!(text(&checked.stdout).contains("Parser Error"));
}

fn check_json(
    compiler: &Path,
    project: &Project,
    file: &str,
    marmot_path: &Path,
) -> serde_json::Value {
    let output = Command::new(env!("CARGO_BIN_EXE_marmot"))
        .args(["check", file, "--format", "json"])
        .current_dir(&project.0)
        .env("MARMOTC", compiler)
        .env("MARMOT_PATH", marmot_path)
        .output()
        .unwrap();
    let report: serde_json::Value = serde_json::from_slice(&output.stdout)
        .unwrap_or_else(|_| panic!("{}{}", text(&output.stdout), text(&output.stderr)));
    assert!(output.status.success(), "{report}");
    report
}

#[test]
fn the_project_source_directory_wins_over_marmot_path() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("precedence");
    // If MARMOT_PATH won, Support::Value would not be callable.
    project.write(
        "external/Support.mmt",
        "module Support\npublic export { Value }\ndef Value = 7;\n",
    );
    project.write(
        "App/project.marmot",
        "[project]\nname = \"App\"\nsource_dir = \"src\"\n",
    );
    project.write(
        "App/src/Support.mmt",
        "module Support\npublic export { Value }\ndef Value = fn() -> Int => 41;\n",
    );
    project.write(
        "App/src/Main.mmt",
        "module Main\nimport { <Support> }\ndef main = fn() -> Int => Support::Value();\n",
    );

    let report = check_json(
        &compiler,
        &project,
        "App/src/Main.mmt",
        &project.path("external"),
    );
    assert_eq!(report["report"]["errors"], serde_json::json!([]));

    // A project.marmot beside a package.marmot wins over it.
    project.write("App/package.marmot", "[project]\nsource_dir = \"pkgsrc\"\n");
    project.write(
        "App/pkgsrc/Support.mmt",
        "module Support\npublic export { Value }\ndef Value = 5;\n",
    );
    let report = check_json(
        &compiler,
        &project,
        "App/src/Main.mmt",
        &project.path("external"),
    );
    assert_eq!(report["report"]["errors"], serde_json::json!([]));
}

#[test]
fn a_package_manifest_with_a_project_table_is_a_project() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("fallback");
    project.write(
        "external/Support.mmt",
        "module Support\npublic export { Value }\ndef Value = 0;\n",
    );
    project.write("Lib/package.marmot", "[project]\nsource_dir = \"libsrc\"\n");
    project.write(
        "Lib/libsrc/Support.mmt",
        "module Support\npublic export { Value }\ndef Value = fn() -> Int => 99;\n",
    );
    project.write(
        "Lib/libsrc/Main.mmt",
        "module Main\nimport { <Support> }\ndef main = fn() -> Int => Support::Value();\n",
    );

    let report = check_json(
        &compiler,
        &project,
        "Lib/libsrc/Main.mmt",
        &project.path("external"),
    );
    assert_eq!(report["report"]["errors"], serde_json::json!([]));
    assert_eq!(report["report"]["warnings"], serde_json::json!([]));
}

#[test]
fn init_scaffolds_projects_and_packages() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("init");

    let created = marmot(
        &compiler,
        &project.0,
        &["init", "CliProject", "--name", "CliProject"],
    );
    assert!(text(&succeeded(&created).stdout).starts_with("Initialized Marmot project at "));
    assert_eq!(
        project.read("CliProject/project.marmot"),
        "[project]\nname = \"CliProject\"\nentry = \"src/Main.mmt\"\nsource_dir = \"src\"\npackages_dir = \"packages\"\nprelude_dir = \"MarmotPrelude\"\n\n[test]\ndir = \"test\"\ntimeout_ms = 30000\n"
    );
    assert_eq!(
        project.read("CliProject/src/Main.mmt"),
        "module Main\n\ndef main = fn() -> Int => 0;\n"
    );
    assert!(
        project.path("CliProject/packages").is_dir() && project.path("CliProject/test").is_dir()
    );
    assert_eq!(project.read("CliProject/.gitignore"), "/target/\n");
    succeeded(&marmot(&compiler, &project.path("CliProject"), &["run"]));
    assert!(project.path("CliProject/target/src/Main.mmc").exists());

    let again = marmot(&compiler, &project.0, &["init", "CliProject"]);
    assert!(!again.status.success());
    assert!(text(&again.stderr).contains("project.marmot already exists"));

    let package = marmot(
        &compiler,
        &project.0,
        &[
            "init",
            "--package",
            "CliPackage",
            "--name",
            "123-demo",
            "--format",
            "json",
        ],
    );
    let payload: serde_json::Value = serde_json::from_slice(&succeeded(&package).stdout).unwrap();
    assert_eq!(payload["success"], serde_json::Value::Bool(true));
    assert_eq!(payload["kind"], "package");
    let manifest = project.read("CliPackage/package.marmot");
    assert!(
        manifest.contains("main = \"Package123_demo.mmt\""),
        "{manifest}"
    );
    assert!(
        manifest.contains("exports = [\"Package123_demo\"]"),
        "{manifest}"
    );
    assert!(project.path("CliPackage/Package123_demo.mmt").exists());
}

#[test]
fn test_runs_the_projects_tests_with_its_packages() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("test");
    project.write(
        "project.marmot",
        "[project]\nentry = \"src/Main.mmt\"\nmarmot_path = [\"registry\"]\n\n[dependencies]\nGreeter = \"^1.0.0\"\n\n[test]\ndir = \"checks\"\ntimeout_ms = 2000\n",
    );
    project.write(
        "checks/greets.mmt",
        "module Greets\nimport { \"<Greeter>\" }\ndef main = fn() -> Int => 0;\ndef greeting = Greeter::Hello(\"test\");\n",
    );
    project.write(
        "checks/hangs.mmt",
        "module Hangs\ndef Spin = fn(n: Int) -> Int => Spin(n + 1);\nSpin(0);\n",
    );

    let tested = marmot(&compiler, &project.0, &["test", "--format", "json"]);
    assert!(!tested.status.success());
    let payload: serde_json::Value = serde_json::from_slice(&tested.stdout)
        .unwrap_or_else(|_| panic!("{}{}", text(&tested.stdout), text(&tested.stderr)));
    assert_eq!(payload["summary"]["total"], 2, "{payload}");
    assert_eq!(payload["summary"]["passed"], 1, "{payload}");
    assert_eq!(payload["summary"]["timedOut"], 1, "{payload}");

    let one = marmot(&compiler, &project.0, &["test", "--test", "greets.mmt"]);
    succeeded(&one);
}

#[test]
fn fmt_is_the_compilers() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("fmt");
    project.write("Main.mmt", "module Main\ndef   main = fn() -> Int => 0;\n");

    let checked = marmot(&compiler, &project.0, &["fmt", "Main.mmt", "--check"]);
    let direct = marmotc(&compiler, &project.0, &["fmt", "Main.mmt", "--check"]);
    assert_eq!(checked.status.code(), direct.status.code());
    assert!(!checked.status.success());

    succeeded(&marmot(&compiler, &project.0, &["fmt", "Main.mmt", "-w"]));
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["fmt", "Main.mmt", "--check"],
    ));
}

#[test]
fn fmt_without_a_path_formats_the_project_but_not_other_peoples_code() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("fmt-project");
    succeeded(&marmot(&compiler, &project.0, &["install"]));
    let messy = "module Main\ndef   main = fn() -> Int => 0;\n";
    project.write("src/Main.mmt", messy);
    project.write("test/Smoke.mmt", messy);
    project.write(".hidden/Skip.mmt", messy);
    project.write("registry/Punctuation/Extra.mmt", messy);
    project.write("MarmotPrelude/Extra.mmt", messy);
    let installed = project.read("packages/Greeter-1.2.0/Greeter.mmt");
    project.write(
        "packages/Greeter-1.2.0/Greeter.mmt",
        &(installed.clone() + "def   x = 1;\n"),
    );

    let checked = marmot(&compiler, &project.0, &["fmt", "--check"]);
    assert!(!checked.status.success());
    assert_eq!(
        project.read("src/Main.mmt"),
        messy,
        "--check must not write"
    );

    succeeded(&marmot(&compiler, &project.path("src"), &["fmt"]));
    assert_ne!(project.read("src/Main.mmt"), messy);
    assert_ne!(project.read("test/Smoke.mmt"), messy);
    assert_eq!(project.read(".hidden/Skip.mmt"), messy);
    assert_eq!(project.read("MarmotPrelude/Extra.mmt"), messy);
    assert_eq!(project.read("registry/Punctuation/Extra.mmt"), messy);
    assert_eq!(
        project.read("packages/Greeter-1.2.0/Greeter.mmt"),
        installed + "def   x = 1;\n"
    );
    succeeded(&marmot(&compiler, &project.0, &["fmt", "--check"]));
}

#[test]
fn run_in_json_reports_the_builds_warnings_and_the_runs_errors() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("run-json");
    project.write(
        "Main.mmt",
        "module Main\nimport { \"<IO>\" }\ndef main = fn() -> Int => {\n    def unused = 1;\n    0\n};\nIO::PrintLine(\"before\");\ndef xs = [1];\nIO::PrintLine((xs[3]) as Text);\n",
    );

    let ran = marmot(
        &compiler,
        &project.0,
        &["run", "Main.mmt", "--format", "json"],
    );
    assert!(!ran.status.success());
    let payload: serde_json::Value = serde_json::from_slice(&ran.stdout)
        .unwrap_or_else(|_| panic!("{}{}", text(&ran.stdout), text(&ran.stderr)));
    assert_eq!(payload["command"], "run");
    assert_eq!(payload["success"], false);
    assert_eq!(payload["stdout"], "before\n");
    assert_eq!(payload["report"]["warnings"][0]["code"], "UnusedLocal");
    assert_eq!(payload["report"]["errors"][0]["code"], "IndexOutOfBounds");
    // Outside a project the program is built somewhere temporary.
    assert!(!project.path("target").exists());
    assert!(!project.path("Main.mmc").exists());

    project.write("Broken.mmt", "module Broken\ndef broken = ;\n");
    let broken = marmot(
        &compiler,
        &project.0,
        &["run", "Broken.mmt", "--format", "json"],
    );
    let payload: serde_json::Value = serde_json::from_slice(&broken.stdout).unwrap();
    assert_eq!(payload["command"], "run");
    assert_eq!(payload["report"]["errors"][0]["stage"], "Parser");
}

#[test]
fn run_hands_the_vm_a_packages_native_library() {
    let Some(compiler) = compiler() else { return };
    // The test library the compiler's build makes beside it.
    let library = if cfg!(windows) {
        compiler.with_file_name("marmot_test_native.dll")
    } else if cfg!(target_os = "macos") {
        compiler.with_file_name("libmarmot_test_native.dylib")
    } else {
        compiler.with_file_name("libmarmot_test_native.so")
    };
    if !library.is_file() {
        eprintln!("skipped: no test library at {}", library.display());
        return;
    }

    let project = Project::new("run-native");
    project.write(
        "project.marmot",
        "[project]\nentry = \"src/Main.mmt\"\nmarmot_path = [\"registry\"]\n\n[dependencies]\nNative = \"^1.0.0\"\n",
    );
    project.write(
        "src/Main.mmt",
        "module Main\nimport { \"<IO>\", \"<Native>\" }\nIO::PrintLine((Native::Answer()) as Text);\n",
    );
    // A prebuilt library under a name no search would find: only the tool's
    // --library can hand it over.
    let prebuilt = "path = \"bin/prebuilt.bin\"\n";
    project.write(
        "registry/Native/package.marmot",
        &format!(
            "[package]\nname = \"Native\"\nversion = \"1.0.0\"\n\n[ffi]\nenabled = true\nlibrary_name = \"marmot_test_native\"\n\n[prebuilt.windows_x64]\n{prebuilt}\n[prebuilt.linux_x86_64]\n{prebuilt}\n[prebuilt.macos_arm64]\n{prebuilt}\n[prebuilt.macos_x86_64]\n{prebuilt}"
        ),
    );
    project.write(
        "registry/Native/Native.mmt",
        "module Native\npublic export { Answer }\nforeign \"marmot_test_answer\" Answer : fn() -> Int from \"marmot_test_native\";\n",
    );
    std::fs::create_dir_all(project.path("registry/Native/bin")).unwrap();
    std::fs::copy(&library, project.path("registry/Native/bin/prebuilt.bin")).unwrap();

    let ran = marmot(&compiler, &project.0, &["run"]);
    assert_eq!(text(&succeeded(&ran).stdout), "42\n");
}
