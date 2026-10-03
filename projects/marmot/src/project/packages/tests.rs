use super::{Mode, prepare, remove_unused};
use crate::project::manifest;
use crate::test_support::{TempTree, compiler, package, project};

#[test]
fn removing_unused_packages_keeps_the_active_versions() {
    let tree = TempTree::new(&[
        ("project.marmot", &project(&[("Lib", "^1.0.0")])),
        ("src/Main.mmt", "module Main\n"),
        ("registry/Lib/package.marmot", &package("Lib", "1.0.0", &[])),
        (
            "packages/Lib-0.9.0/package.marmot",
            &package("Lib", "0.9.0", &[]),
        ),
        (
            "packages/Other-1.0.0/package.marmot",
            &package("Other", "1.0.0", &[]),
        ),
    ]);
    let workspace = manifest::find_workspace(&tree.0).unwrap().unwrap();
    let prepared = prepare(&workspace, Mode::ForceRefresh, &[], &compiler()).unwrap();

    remove_unused(&workspace, &prepared.graph, Some("Lib"), &compiler()).unwrap();

    assert!(tree.path("packages/Lib-1.0.0").exists());
    assert!(!tree.path("packages/Lib-0.9.0").exists());
    assert!(tree.path("packages/Other-1.0.0").exists());
}
