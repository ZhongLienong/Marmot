use super::{Options, print_warnings};
use crate::paths;
use crate::project::manifest::current_workspace;
use crate::project::packages::{self, Mode};
use crate::project::version::Version;
use crate::project::{edit, lockfile, resolver};

pub(super) fn install(options: &Options, version: &Version) -> Result<(), String> {
    let environment = paths::marmot_path();
    let mut workspace = current_workspace()?;
    let mut added = None;
    if let Some(name) = &options.argument {
        let constraint = match &options.constraint {
            Some(constraint) => constraint.clone(),
            None => format!(
                "^{}",
                packages::newest_version(&workspace, name, &environment, version)?
            ),
        };
        edit::add_dependency(&workspace.manifest_path, name, &constraint)?;
        workspace = current_workspace()?;
        added = Some((name.clone(), constraint));
    }

    let prepared = packages::prepare(&workspace, Mode::ForceRefresh, &environment, version)?;
    print_warnings(&prepared.warnings);
    println!(
        "Installed {} package(s); lockfile updated at {}",
        prepared.graph.packages.len(),
        prepared.lockfile_path.display()
    );
    if let Some((name, constraint)) = added {
        println!("Added dependency {name} {constraint}");
    }
    if !prepared.graph.roots.is_empty() {
        print!("{}", prepared.graph.render_tree());
    }
    Ok(())
}

pub(super) fn update(options: &Options, version: &Version) -> Result<(), String> {
    let workspace = current_workspace()?;
    if let Some(name) = &options.argument
        && !workspace.dependencies.contains_key(name)
    {
        return Err(format!("Package '{name}' is not a direct dependency."));
    }

    let before = lockfile::read(&workspace.root, version).map(|lock| lock.graph);
    let prepared = packages::prepare(
        &workspace,
        Mode::ForceRefresh,
        &paths::marmot_path(),
        version,
    )?;
    print_warnings(&prepared.warnings);

    let mut names: Vec<&String> = before
        .iter()
        .flat_map(|graph| graph.packages.keys())
        .chain(prepared.graph.packages.keys())
        .collect();
    names.sort();
    names.dedup();
    let version_in = |graph: Option<&resolver::Graph>, name: &str| {
        graph
            .and_then(|graph| graph.packages.get(name))
            .map(|package| package.manifest.version_text.clone())
            .unwrap_or_else(|| "<none>".to_string())
    };
    let changes: String = names
        .into_iter()
        .filter_map(|name| {
            let (old, new) = (
                version_in(before.as_ref(), name),
                version_in(Some(&prepared.graph), name),
            );
            (old != new).then(|| format!("{name}: {old} -> {new}\n"))
        })
        .collect();

    if changes.is_empty() {
        println!("No package versions changed.");
    } else {
        print!("Updated packages:\n{changes}");
    }
    Ok(())
}

pub(super) fn remove(options: &Options, version: &Version) -> Result<(), String> {
    let name = options.argument.as_deref().unwrap_or_default();
    let workspace = current_workspace()?;
    edit::remove_dependency(&workspace.manifest_path, name)?;
    let workspace = current_workspace()?;

    let prepared = packages::prepare(
        &workspace,
        Mode::ForceRefresh,
        &paths::marmot_path(),
        version,
    )?;
    packages::remove_unused(&workspace, &prepared.graph, Some(name), version)?;
    print_warnings(&prepared.warnings);
    println!("Removed dependency {name}");
    if !prepared.graph.roots.is_empty() {
        print!("{}", prepared.graph.render_tree());
    }
    Ok(())
}

pub(super) fn list(version: &Version) -> Result<(), String> {
    let workspace = current_workspace()?;
    let prepared = packages::prepare(
        &workspace,
        Mode::PreferLockfile,
        &paths::marmot_path(),
        version,
    )?;
    print_warnings(&prepared.warnings);
    if prepared.graph.roots.is_empty() {
        println!("No dependencies resolved.");
    } else {
        print!("{}", prepared.graph.render_tree());
    }
    Ok(())
}
