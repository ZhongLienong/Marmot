use crate::lockfile::{self, LockedPackage};
use crate::manifest::Workspace;
use crate::paths;
use crate::plan::{self, Plan, Resolver};
use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

struct TempTree(PathBuf);

impl TempTree {
    fn new(files: &[(&str, &str)]) -> TempTree {
        static COUNTER: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "marmot-tool-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::SeqCst)
        ));
        let _ = std::fs::remove_dir_all(&root);
        for (relative, contents) in files {
            let path = root.join(relative);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, contents).unwrap();
        }
        std::fs::create_dir_all(&root).unwrap();
        TempTree(root)
    }

    fn path(&self, relative: &str) -> PathBuf {
        self.0.join(relative)
    }

    fn write(&self, relative: &str, contents: &str) {
        let path = self.path(relative);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, contents).unwrap();
    }
}

impl Drop for TempTree {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Stands in for `marmotc install`: counts calls and optionally writes a lockfile.
struct FakeResolver<F: Fn(&Workspace)> {
    calls: Cell<usize>,
    action: F,
}

impl<F: Fn(&Workspace)> Resolver for FakeResolver<F> {
    fn resolve(&self, workspace: &Workspace, _reason: &str) -> Result<(), String> {
        self.calls.set(self.calls.get() + 1);
        (self.action)(workspace);
        Ok(())
    }
}

fn no_resolution() -> FakeResolver<impl Fn(&Workspace)> {
    FakeResolver {
        calls: Cell::new(0),
        action: |_: &Workspace| {},
    }
}

fn package_manifest(name: &str, ffi: &str) -> String {
    format!("[package]\nname = \"{name}\"\nversion = \"1.0.0\"\n{ffi}")
}

fn lockfile(tree: &TempTree, manifest: &str, packages: &[(&str, &str, &[&str])]) -> String {
    let checksum = lockfile::file_checksum(&tree.path(manifest)).unwrap();
    let mut text = format!("[metadata]\nmanifest_checksum = \"{checksum}\"\n");
    for (name, source, dependencies) in packages {
        let dependencies: Vec<String> = dependencies
            .iter()
            .map(|dependency| format!("\"{dependency}@1.0.0\""))
            .collect();
        text += &format!(
            "\n[[package]]\nname = \"{name}\"\nversion = \"1.0.0\"\nsource = \"{source}\"\nchecksum = \"\"\ndependencies = [{}]\n",
            dependencies.join(", ")
        );
    }
    text
}

fn shown(path: &Path) -> String {
    paths::display(path)
}

#[test]
fn without_a_manifest_the_search_paths_are_marmot_path() {
    let tree = TempTree::new(&[
        ("app/Main.mmt", "module Main\n"),
        ("lib/Lib.mmt", "module Lib\n"),
    ]);
    let environment = vec![
        tree.path("missing"),
        tree.path("lib"),
        tree.path("lib/../lib"),
    ];

    let plan = plan::make_plan(&tree.path("app/Main.mmt"), &environment, &no_resolution()).unwrap();

    assert_eq!(plan.entry, shown(&tree.path("app/Main.mmt")));
    assert_eq!(plan.search_paths, vec![shown(&tree.path("lib"))]);
    assert!(plan.native_packages.is_empty());
}

#[test]
fn a_project_orders_source_packages_extra_paths_prelude_then_marmot_path() {
    let tree = TempTree::new(&[
        (
            "project.marmot",
            "[project]\nentry = \"src/Main.mmt\"\nmarmot_path = [\"shared\"]\n\n[dependencies]\nApp = \"^1.0.0\"\n",
        ),
        ("src/Main.mmt", "module Main\n"),
        ("shared/Shared.mmt", "module Shared\n"),
        ("MarmotPrelude/IO.mmt", "module IO\n"),
        (
            "packages/Zeta-1.0.0/package.marmot",
            &package_manifest("Zeta", ""),
        ),
        (
            "packages/Alpha-1.0.0/package.marmot",
            &package_manifest("Alpha", ""),
        ),
        (
            "packages/App-1.0.0/package.marmot",
            &package_manifest("App", ""),
        ),
        ("env/Env.mmt", "module Env\n"),
    ]);
    // App depends on Zeta and Alpha; with both ready, Alpha sorts first.
    tree.write(
        "marmot.lock",
        &lockfile(
            &tree,
            "project.marmot",
            &[
                ("App", "local:packages/App-1.0.0", &["Zeta", "Alpha"]),
                ("Zeta", "local:packages/Zeta-1.0.0", &[]),
                ("Alpha", "local:packages/Alpha-1.0.0", &[]),
            ],
        ),
    );

    let resolver = no_resolution();
    let plan = plan::make_plan(
        &tree.path("src/Main.mmt"),
        &[tree.path("env"), tree.path("src")],
        &resolver,
    )
    .unwrap();

    assert_eq!(resolver.calls.get(), 0);
    assert_eq!(
        plan.search_paths,
        vec![
            shown(&tree.path("src")),
            shown(&tree.path("packages/Alpha-1.0.0")),
            shown(&tree.path("packages/Zeta-1.0.0")),
            shown(&tree.path("packages/App-1.0.0")),
            shown(&tree.path("shared")),
            shown(&tree.path("MarmotPrelude")),
            shown(&tree.path("env")),
        ]
    );
}

#[test]
fn a_stale_lockfile_is_resolved_once_and_then_read() {
    let tree = TempTree::new(&[
        (
            "project.marmot",
            "[project]\n\n[dependencies]\nNative = \"^1.0.0\"\n",
        ),
        ("src/Main.mmt", "module Main\n"),
        (
            "packages/Native-1.0.0/package.marmot",
            &package_manifest(
                "Native",
                "\n[ffi]\nenabled = true\nlibrary_name = \"native\"\nthread_safe = true\n\n[ffi.functions]\n\"MIDORI_FFI_Native_Answer\" = \"native_answer\"\n\n[prebuilt.windows_x64]\npath = \"bin/native.dll\"\nchecksum = \"sha256:ab\"\n",
            ),
        ),
    ]);

    let resolver = FakeResolver {
        calls: Cell::new(0),
        action: |workspace: &Workspace| {
            let checksum = lockfile::file_checksum(&workspace.manifest_path).unwrap();
            std::fs::write(
                workspace.root.join("marmot.lock"),
                format!(
                    "[metadata]\nmanifest_checksum = \"{checksum}\"\n\n[[package]]\nname = \"Native\"\nversion = \"1.0.0\"\nsource = \"local:packages/Native-1.0.0\"\n"
                ),
            )
            .unwrap();
        },
    };
    let plan = plan::make_plan(&tree.path("src/Main.mmt"), &[], &resolver).unwrap();

    assert_eq!(resolver.calls.get(), 1);
    assert_eq!(
        plan.search_paths,
        vec![
            shown(&tree.path("src")),
            shown(&tree.path("packages/Native-1.0.0"))
        ]
    );
    assert_eq!(plan.native_packages.len(), 1);
    let native = &plan.native_packages[0];
    assert_eq!(native.name, "Native");
    assert_eq!(native.root, shown(&tree.path("packages/Native-1.0.0")));
    assert!(native.thread_safe);
    assert_eq!(
        native
            .functions
            .get("MIDORI_FFI_Native_Answer")
            .map(String::as_str),
        Some("native_answer")
    );
    if cfg!(windows) {
        assert_eq!(
            native.library,
            shown(&tree.path("packages/Native-1.0.0/bin/native.dll"))
        );
        assert_eq!(native.checksum.as_deref(), Some("sha256:ab"));
    }

    // Unchanged manifest and lockfile: no second resolution.
    let again = plan::make_plan(&tree.path("src/Main.mmt"), &[], &resolver).unwrap();
    assert_eq!(resolver.calls.get(), 1);
    assert_eq!(again, plan);
}

#[test]
fn editing_the_manifest_makes_the_lockfile_stale() {
    let tree = TempTree::new(&[
        ("project.marmot", "[project]\n"),
        ("src/Main.mmt", "module Main\n"),
    ]);
    tree.write("marmot.lock", &lockfile(&tree, "project.marmot", &[]));
    tree.write("project.marmot", "[project]\nname = \"Edited\"\n");

    let resolver = no_resolution();
    let error = plan::make_plan(&tree.path("src/Main.mmt"), &[], &resolver).unwrap_err();

    assert_eq!(resolver.calls.get(), 1);
    assert!(error.contains("still unusable"), "{error}");
}

#[test]
fn a_locked_package_at_the_wrong_version_makes_the_lockfile_stale() {
    let tree = TempTree::new(&[
        ("project.marmot", "[project]\n"),
        ("src/Main.mmt", "module Main\n"),
        (
            "packages/Lib/package.marmot",
            "[package]\nname = \"Lib\"\nversion = \"2.0.0\"\n",
        ),
    ]);
    tree.write(
        "marmot.lock",
        &lockfile(
            &tree,
            "project.marmot",
            &[("Lib", "local:packages/Lib", &[])],
        ),
    );

    let resolver = no_resolution();
    let error = plan::make_plan(&tree.path("src/Main.mmt"), &[], &resolver).unwrap_err();
    assert_eq!(resolver.calls.get(), 1);
    assert!(
        error.contains("expected version 1.0.0, but found 2.0.0"),
        "{error}"
    );
}

#[test]
fn a_package_manifest_is_a_workspace_rooted_at_itself() {
    let tree = TempTree::new(&[
        (
            "Pkg/package.marmot",
            &package_manifest(
                "Pkg",
                "\n[ffi]\nenabled = true\nlibrary_name = \"pkg\"\n\n[ffi.functions]\n\"F\" = \"f\"\n",
            ),
        ),
        ("Pkg/Pkg.mmt", "module Pkg\n"),
    ]);
    tree.write(
        "Pkg/marmot.lock",
        &lockfile(&tree, "Pkg/package.marmot", &[]),
    );

    let plan = plan::make_plan(&tree.path("Pkg/Pkg.mmt"), &[], &no_resolution()).unwrap();

    assert_eq!(plan.search_paths, vec![shown(&tree.path("Pkg"))]);
    assert_eq!(plan.native_packages.len(), 1);
    assert_eq!(plan.native_packages[0].name, "Pkg");
}

#[test]
fn a_package_without_ffi_or_with_another_abi_is_not_native() {
    let tree = TempTree::new(&[
        ("app/Main.mmt", "module Main\n"),
        ("plain/package.marmot", &package_manifest("Plain", "")),
        (
            "old/package.marmot",
            &package_manifest(
                "Old",
                "\n[ffi]\nenabled = true\nlibrary_name = \"old\"\nabi_version = 2\n",
            ),
        ),
    ]);

    let plan = plan::make_plan(
        &tree.path("app/Main.mmt"),
        &[tree.path("plain"), tree.path("old")],
        &no_resolution(),
    )
    .unwrap();

    assert_eq!(plan.search_paths.len(), 2);
    assert!(plan.native_packages.is_empty());
}

#[test]
fn project_manifest_without_a_project_table_is_an_error() {
    let tree = TempTree::new(&[
        ("project.marmot", "[dependencies]\n"),
        ("src/Main.mmt", "module Main\n"),
    ]);
    let error = plan::make_plan(&tree.path("src/Main.mmt"), &[], &no_resolution()).unwrap_err();
    assert!(error.contains("missing [project] table"), "{error}");
}

#[test]
fn packages_in_a_cycle_are_left_out_like_the_compiler_does() {
    let package = |name: &str, dependencies: &[&str]| LockedPackage {
        name: name.to_string(),
        directory: PathBuf::from(name),
        dependencies: dependencies
            .iter()
            .map(|dependency| dependency.to_string())
            .collect(),
    };
    let packages = vec![
        package("C", &["B"]),
        package("B", &["C"]),
        package("A", &["Missing"]),
        package("D", &["A"]),
    ];

    let order: Vec<&str> = lockfile::topological_order(&packages)
        .iter()
        .map(|package| package.name.as_str())
        .collect();
    assert_eq!(order, vec!["A", "D"]);
}

#[test]
fn the_plan_serialises_as_the_compiler_expects() {
    let plan = Plan {
        version: 1,
        entry: "E".into(),
        search_paths: vec!["S".into()],
        native_packages: Vec::new(),
    };
    let json: serde_json::Value = serde_json::from_str(&plan.to_json()).unwrap();
    assert_eq!(
        json,
        serde_json::json!({ "version": 1, "entry": "E", "search_paths": ["S"], "native_packages": [] })
    );
}
