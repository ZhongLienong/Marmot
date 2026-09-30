use crate::manifest::Workspace;
use crate::paths;
use std::path::{Path, PathBuf};

/// Whether `marmot fmt`'s arguments name something to format. `--format`
/// takes a value, which is not a path.
pub fn names_a_path(args: &[String]) -> bool {
    let mut index = 0;
    while index < args.len() {
        let arg = args[index].as_str();
        if arg == "--format" {
            index += 2;
            continue;
        }
        if !arg.starts_with('-') {
            return true;
        }
        index += 1;
    }
    false
}

/// What a bare `marmot fmt` formats: the project root's source files and
/// folders, except hidden folders and other people's code: the installed
/// packages (vendored copies whose checksums the lockfile records), the
/// prelude and the `marmot_path` package registries.
pub fn project_sources(workspace: &Workspace) -> Result<Vec<PathBuf>, String> {
    let entries = std::fs::read_dir(&workspace.root)
        .map_err(|error| format!("cannot read {}: {error}", workspace.root.display()))?;
    let excluded: Vec<String> = [&workspace.packages_dir, &workspace.prelude_dir]
        .into_iter()
        .chain(&workspace.extra_paths)
        .map(|directory| paths::identity_key(&paths::under(&workspace.root, directory)))
        .collect();
    let mut sources = Vec::new();
    for entry in entries {
        let path = entry
            .map_err(|error| format!("cannot read {}: {error}", workspace.root.display()))?
            .path();
        if path.is_dir() {
            if !is_hidden(&path) && !excluded.contains(&paths::identity_key(&path)) {
                sources.push(path);
            }
        } else if path.extension().is_some_and(|extension| extension == "mmt") {
            sources.push(path);
        }
    }
    sources.sort();
    Ok(sources)
}

fn is_hidden(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.starts_with('.'))
}

/// A bare `marmot fmt` writes, as `cargo fmt` does, unless asked only to check.
pub fn writes_by_default(args: &[String]) -> bool {
    !args
        .iter()
        .any(|arg| arg == "--check" || arg == "--write" || arg == "-w")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|arg| arg.to_string()).collect()
    }

    #[test]
    fn a_format_value_is_not_a_path() {
        assert!(!names_a_path(&args(&["--check", "--format", "json"])));
        assert!(names_a_path(&args(&["--format", "json", "src"])));
        assert!(names_a_path(&args(&["Main.mmt", "-w"])));
    }

    #[test]
    fn a_bare_fmt_writes_unless_it_only_checks() {
        assert!(writes_by_default(&args(&[])));
        assert!(writes_by_default(&args(&["--format", "json"])));
        assert!(!writes_by_default(&args(&["--check"])));
        assert!(!writes_by_default(&args(&["-w"])));
    }
}
