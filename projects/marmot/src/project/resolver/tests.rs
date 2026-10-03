use super::{Graph, Index, ResolvedPackage, resolve};
use crate::paths;
use crate::project::manifest;
use crate::project::version::Constraint;
use crate::test_support::{TempTree, compiler, package};
use std::collections::BTreeMap;

fn index(tree: &TempTree) -> Index {
    Index::scan(&[tree.path("registry"), tree.path("second")], &compiler()).unwrap()
}

fn roots(entries: &[(&str, &str)]) -> BTreeMap<String, Constraint> {
    entries
        .iter()
        .map(|(name, constraint)| (name.to_string(), Constraint::parse(constraint).unwrap()))
        .collect()
}

#[test]
fn the_resolver_picks_the_newest_match_and_prefers_earlier_roots() {
    let tree = TempTree::new(&[
        ("registry/A1/package.marmot", &package("A", "1.2.0", &[])),
        ("registry/A2/package.marmot", &package("A", "1.9.0", &[])),
        ("registry/A3/package.marmot", &package("A", "2.0.0", &[])),
        ("second/A/package.marmot", &package("A", "1.9.0", &[])),
    ]);

    let graph = resolve(&mut index(&tree), &roots(&[("A", "^1.0.0")])).unwrap();
    let chosen = &graph.packages["A"].manifest;
    assert_eq!(chosen.version_text, "1.9.0");
    assert_eq!(
        paths::identity_key(&chosen.directory),
        paths::identity_key(&tree.path("registry/A2"))
    );
}

#[test]
fn the_resolver_reports_conflicts_missing_packages_and_cycles() {
    let tree = TempTree::new(&[
        (
            "registry/A/package.marmot",
            &package("A", "1.0.0", &[("C", "^1.0.0")]),
        ),
        (
            "registry/B/package.marmot",
            &package("B", "1.0.0", &[("C", "^2.0.0")]),
        ),
        ("registry/C/package.marmot", &package("C", "1.0.0", &[])),
        (
            "registry/X/package.marmot",
            &package("X", "1.0.0", &[("Y", "^1.0.0")]),
        ),
        (
            "registry/Y/package.marmot",
            &package("Y", "1.0.0", &[("X", "^1.0.0")]),
        ),
    ]);
    let mut index = index(&tree);

    let conflict = resolve(&mut index, &roots(&[("A", "^1.0.0"), ("B", "^1.0.0")])).unwrap_err();
    assert_eq!(
        conflict,
        "Version conflict for package 'C': B requires '^2.0.0', but the resolved version is 1.0.0."
    );

    let missing = resolve(&mut index, &roots(&[("Nope", "^1.0.0")])).unwrap_err();
    assert_eq!(
        missing,
        "Could not resolve package 'Nope' required by <root> with constraint '^1.0.0'."
    );

    let cycle = resolve(&mut index, &roots(&[("X", "^1.0.0")])).unwrap_err();
    assert_eq!(cycle, "Detected a package dependency cycle: X -> Y -> X");
}

#[test]
fn a_package_for_another_compiler_version_stops_the_scan() {
    let tree = TempTree::new(&[(
        "registry/Future/package.marmot",
        "[package]\nname = \"Future\"\nversion = \"1.0.0\"\nmarmot_version = \">=2.0.0\"\n",
    )]);
    let error = Index::scan(&[tree.path("registry")], &compiler())
        .err()
        .unwrap();
    assert_eq!(
        error,
        "Package 'Future' requires Marmot >=2.0.0, but the current compiler version is 1.0.0."
    );
}

#[test]
fn packages_in_a_cycle_are_left_out_like_the_compiler_does() {
    let tree = TempTree::new(&[("p/package.marmot", &package("P", "1.0.0", &[]))]);
    let manifest = manifest::read_package(&tree.path("p"), &compiler()).unwrap();
    let entry = |dependencies: &[&str]| ResolvedPackage {
        manifest: manifest.clone(),
        dependencies: dependencies.iter().map(|name| name.to_string()).collect(),
        source: String::new(),
        checksum: String::new(),
    };
    let graph = Graph {
        packages: [
            ("C", entry(&["B"])),
            ("B", entry(&["C"])),
            ("A", entry(&["Missing"])),
            ("D", entry(&["A"])),
        ]
        .into_iter()
        .map(|(name, package)| (name.to_string(), package))
        .collect(),
        roots: vec!["D".to_string(), "C".to_string()],
    };

    assert_eq!(graph.topological_order(), vec!["A", "D"]);
    assert_eq!(
        graph.render_tree(),
        "D@1.0.0\n  A@1.0.0\n    Missing (missing)\nC@1.0.0\n  B@1.0.0\n    C@1.0.0\n      (cycle)\n"
    );
}
