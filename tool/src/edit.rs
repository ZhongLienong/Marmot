use crate::version::Constraint;
use std::path::Path;
use toml_edit::{DocumentMut, Item, Table, value};

fn load(path: &Path) -> Result<DocumentMut, String> {
    std::fs::read_to_string(path)
        .map_err(|error| format!("cannot read {}: {error}", path.display()))?
        .parse::<DocumentMut>()
        .map_err(|error| format!("Failed to update {}: {error}", path.display()))
}

fn save(path: &Path, document: &DocumentMut) -> Result<(), String> {
    std::fs::write(path, document.to_string())
        .map_err(|error| format!("Failed to write file: {}: {error}", path.display()))
}

/// Sets `name = "constraint"` under `[dependencies]`, keeping the rest of the
/// manifest as written.
pub fn add_dependency(manifest_path: &Path, name: &str, constraint: &str) -> Result<(), String> {
    if name.is_empty() {
        return Err("Package name cannot be empty.".to_string());
    }
    Constraint::parse(constraint)
        .map_err(|error| format!("Invalid version constraint '{constraint}': {error}"))?;

    let mut document = load(manifest_path)?;
    let dependencies = document
        .entry("dependencies")
        .or_insert_with(|| Item::Table(Table::new()))
        .as_table_like_mut()
        .ok_or_else(|| {
            format!(
                "Manifest dependencies table is not a table: {}",
                manifest_path.display()
            )
        })?;
    dependencies.insert(name, value(constraint));
    save(manifest_path, &document)
}

/// Removes `name` from `[dependencies]`, and the table when it empties.
pub fn remove_dependency(manifest_path: &Path, name: &str) -> Result<(), String> {
    if name.is_empty() {
        return Err("Package name cannot be empty.".to_string());
    }

    let mut document = load(manifest_path)?;
    let dependencies = document
        .get_mut("dependencies")
        .and_then(Item::as_table_like_mut)
        .ok_or_else(|| {
            format!(
                "No [dependencies] table found in {}",
                manifest_path.display()
            )
        })?;
    if dependencies.remove(name).is_none() {
        return Err(format!(
            "Dependency '{name}' is not present in {}",
            manifest_path.display()
        ));
    }
    if dependencies.is_empty() {
        document.remove("dependencies");
    }
    save(manifest_path, &document)
}

#[cfg(test)]
mod tests {
    use super::{add_dependency, remove_dependency};

    #[test]
    fn edits_keep_comments_and_layout() {
        let path = std::env::temp_dir().join(format!("marmot-edit-{}.marmot", std::process::id()));
        let original = "# my app\n[project]\nname = \"App\" # the name\n";
        std::fs::write(&path, original).unwrap();

        add_dependency(&path, "Image", "^0.4.0").unwrap();
        add_dependency(&path, "Greeter", ">=1.0.0, <2.0.0").unwrap();
        let added = std::fs::read_to_string(&path).unwrap();
        assert!(added.starts_with(original), "{added}");
        assert!(added.contains("Image = \"^0.4.0\""), "{added}");
        assert!(add_dependency(&path, "Bad", "^x").is_err());

        remove_dependency(&path, "Image").unwrap();
        remove_dependency(&path, "Greeter").unwrap();
        assert_eq!(
            std::fs::read_to_string(&path).unwrap().trim_end(),
            original.trim_end()
        );
        assert!(remove_dependency(&path, "Greeter").is_err());
        let _ = std::fs::remove_file(&path);
    }
}
