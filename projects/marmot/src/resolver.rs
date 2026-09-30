use crate::manifest::{self, PACKAGE_MANIFEST, PackageManifest};
use crate::paths;
use crate::version::{Constraint, Version};
use std::cmp::Reverse;
use std::collections::{BTreeMap, BTreeSet, BinaryHeap};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct ResolvedPackage {
    pub manifest: PackageManifest,
    /// Names of the packages this one depends on, sorted.
    pub dependencies: Vec<String>,
    /// Where the lockfile says the package lives (`local:<path>`); empty until
    /// the package is installed into the project.
    pub source: String,
    /// The package's source checksum; empty when not yet computed.
    pub checksum: String,
}

#[derive(Debug, Clone, Default)]
pub struct Graph {
    pub packages: BTreeMap<String, ResolvedPackage>,
    /// The packages the project depends on directly, sorted.
    pub roots: Vec<String>,
}

impl Graph {
    /// Dependencies before dependents; among packages ready at the same time
    /// the alphabetically first goes first. Packages caught in a cycle are
    /// left out, as the compiler's resolver does.
    pub fn topological_order(&self) -> Vec<&str> {
        let mut in_degree: BTreeMap<&str, usize> = self
            .packages
            .keys()
            .map(|name| (name.as_str(), 0))
            .collect();
        let mut dependents: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
        for (name, package) in &self.packages {
            for dependency in &package.dependencies {
                if self.packages.contains_key(dependency) {
                    dependents
                        .entry(dependency.as_str())
                        .or_default()
                        .push(name.as_str());
                    *in_degree.entry(name.as_str()).or_default() += 1;
                }
            }
        }

        let mut ready: BinaryHeap<Reverse<&str>> = in_degree
            .iter()
            .filter(|(_, degree)| **degree == 0)
            .map(|(name, _)| Reverse(*name))
            .collect();
        let mut order = Vec::new();
        while let Some(Reverse(name)) = ready.pop() {
            order.push(name);
            for dependent in dependents.get(name).map(Vec::as_slice).unwrap_or_default() {
                let degree = in_degree
                    .get_mut(dependent)
                    .expect("every dependent is a package");
                *degree -= 1;
                if *degree == 0 {
                    ready.push(Reverse(dependent));
                }
            }
        }
        order
    }

    /// The directories of the packages, in dependency order.
    pub fn search_paths(&self) -> Vec<PathBuf> {
        self.topological_order()
            .into_iter()
            .map(|name| self.packages[name].manifest.directory.clone())
            .collect()
    }

    /// `Name@version` per package, dependencies indented under each root.
    pub fn render_tree(&self) -> String {
        fn render(
            graph: &Graph,
            name: &str,
            indent: &str,
            visiting: &mut BTreeSet<String>,
            output: &mut String,
        ) {
            let Some(package) = graph.packages.get(name) else {
                output.push_str(&format!("{indent}{name} (missing)\n"));
                return;
            };
            output.push_str(&format!(
                "{indent}{name}@{}\n",
                package.manifest.version_text
            ));
            if !visiting.insert(name.to_string()) {
                output.push_str(&format!("{indent}  (cycle)\n"));
                return;
            }
            for dependency in &package.dependencies {
                render(graph, dependency, &format!("{indent}  "), visiting, output);
            }
            visiting.remove(name);
        }

        let mut output = String::new();
        let mut visiting = BTreeSet::new();
        for root in &self.roots {
            render(self, root, "", &mut visiting, &mut output);
        }
        output
    }
}

/// Every package found in the index roots, newest version first.
pub struct Index {
    packages: BTreeMap<String, Vec<(PackageManifest, usize)>>,
}

fn candidate_directories(root: &Path) -> Vec<PathBuf> {
    let mut directories = Vec::new();
    if !root.is_dir() {
        return directories;
    }
    if root.join(PACKAGE_MANIFEST).exists() {
        directories.push(root.to_path_buf());
    }
    if let Ok(entries) = std::fs::read_dir(root) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() && path.join(PACKAGE_MANIFEST).exists() {
                directories.push(path);
            }
        }
    }
    directories
}

impl Index {
    /// A root is a package itself or a directory of packages, one level deep.
    /// Earlier roots win when two have the same version of a package.
    pub fn scan(roots: &[PathBuf], compiler: &Version) -> Result<Index, String> {
        let mut packages: BTreeMap<String, Vec<(PackageManifest, usize)>> = BTreeMap::new();
        let mut visited = BTreeSet::new();
        for (priority, root) in roots.iter().enumerate() {
            for directory in candidate_directories(root) {
                if !visited.insert(paths::identity_key(&directory)) {
                    continue;
                }
                let manifest = manifest::read_package(&directory, compiler)?;
                packages
                    .entry(manifest.name.clone())
                    .or_default()
                    .push((manifest, priority));
            }
        }

        for entries in packages.values_mut() {
            entries.sort_by(|(left, left_priority), (right, right_priority)| {
                right
                    .version
                    .cmp(&left.version)
                    .then(left_priority.cmp(right_priority))
                    .then_with(|| {
                        paths::generic(&left.directory).cmp(&paths::generic(&right.directory))
                    })
            });
        }
        Ok(Index { packages })
    }

    pub fn newest(&self, name: &str) -> Option<&PackageManifest> {
        self.packages
            .get(name)
            .and_then(|entries| entries.first())
            .map(|(manifest, _)| manifest)
    }

    fn best_match(&self, name: &str, constraint: &Constraint) -> Option<&PackageManifest> {
        self.packages.get(name).and_then(|entries| {
            entries
                .iter()
                .map(|(manifest, _)| manifest)
                .find(|manifest| constraint.matches(&manifest.version))
        })
    }
}

/// Picks, for each dependency, the newest version that satisfies its
/// constraint, depth first from the project's dependencies in name order. A
/// later constraint the chosen version does not satisfy is a conflict; there
/// is no backtracking.
pub fn resolve(index: &Index, roots: &BTreeMap<String, Constraint>) -> Result<Graph, String> {
    fn visit(
        index: &Index,
        graph: &mut Graph,
        name: &str,
        constraint: &Constraint,
        required_by: &str,
        stack: &mut Vec<String>,
    ) -> Result<(), String> {
        if stack.iter().any(|entry| entry == name) {
            let chain: Vec<&str> = stack
                .iter()
                .map(String::as_str)
                .chain(std::iter::once(name))
                .collect();
            return Err(format!(
                "Detected a package dependency cycle: {}",
                chain.join(" -> ")
            ));
        }

        if let Some(existing) = graph.packages.get(name) {
            if !constraint.matches(&existing.manifest.version) {
                return Err(format!(
                    "Version conflict for package '{name}': {required_by} requires '{constraint}', but the resolved version is {}.",
                    existing.manifest.version_text
                ));
            }
            return Ok(());
        }

        let manifest = index.best_match(name, constraint).ok_or_else(|| {
            format!(
                "Could not resolve package '{name}' required by {required_by} with constraint '{constraint}'."
            )
        })?;
        let dependencies: Vec<String> = manifest.dependencies.keys().cloned().collect();
        graph.packages.insert(
            name.to_string(),
            ResolvedPackage {
                manifest: manifest.clone(),
                dependencies: dependencies.clone(),
                source: String::new(),
                checksum: String::new(),
            },
        );

        stack.push(name.to_string());
        for dependency in &dependencies {
            let result = visit(
                index,
                graph,
                dependency,
                &manifest.dependencies[dependency],
                name,
                stack,
            );
            if result.is_err() {
                graph.packages.remove(name);
                stack.pop();
                return result;
            }
        }
        stack.pop();
        Ok(())
    }

    let mut graph = Graph {
        roots: roots.keys().cloned().collect(),
        ..Graph::default()
    };
    let mut stack = Vec::new();
    for (name, constraint) in roots {
        visit(index, &mut graph, name, constraint, "<root>", &mut stack)?;
    }
    Ok(graph)
}
