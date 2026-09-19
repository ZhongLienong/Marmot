use crate::manifest::{PACKAGE_MANIFEST, PROJECT_MANIFEST};
use crate::paths;
use std::path::{Path, PathBuf};

fn escape_toml(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len());
    for c in value.chars() {
        match c {
            '\\' => escaped.push_str("\\\\"),
            '"' => escaped.push_str("\\\""),
            '\n' => escaped.push_str("\\n"),
            '\r' => escaped.push_str("\\r"),
            '\t' => escaped.push_str("\\t"),
            other => escaped.push(other),
        }
    }
    escaped
}

/// A module name from a package name: bytes that are not ASCII letters,
/// digits or `_` become `_`, and a name that does not start with a letter or
/// `_` gets `Package` in front.
pub fn module_name(name: &str) -> String {
    let mut result: String = name
        .bytes()
        .map(|byte| {
            if byte.is_ascii_alphanumeric() || byte == b'_' {
                byte as char
            } else {
                '_'
            }
        })
        .collect();
    if result.is_empty() {
        result = "Package".to_string();
    }
    if !result.starts_with(|c: char| c.is_ascii_alphabetic() || c == '_') {
        result.insert_str(0, "Package");
    }
    result
}

fn prepare_root(target: Option<&Path>) -> Result<PathBuf, String> {
    let root = match target {
        Some(target) => paths::absolute(target),
        None => std::env::current_dir()
            .map_err(|_| "Failed to resolve current directory.".to_string())?,
    };
    if root.exists() {
        if !root.is_dir() {
            return Err(format!("Path is not a directory: {}", root.display()));
        }
    } else {
        std::fs::create_dir_all(&root)
            .map_err(|_| format!("Failed to create directory: {}", root.display()))?;
    }
    Ok(root)
}

fn derived_name(root: &Path, fallback: &str) -> String {
    root.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .filter(|name| !name.is_empty())
        .unwrap_or_else(|| fallback.to_string())
}

fn write(path: &Path, contents: &str) -> Result<(), String> {
    std::fs::write(path, contents).map_err(|_| format!("Failed to write file: {}", path.display()))
}

fn create_directory(path: &Path) -> Result<(), String> {
    std::fs::create_dir_all(path)
        .map_err(|_| format!("Failed to create directory: {}", path.display()))
}

/// `project.marmot`, `src/Main.mmt`, and empty `packages/` and `test/`.
pub fn project(target: Option<&Path>, name: Option<&str>) -> Result<PathBuf, String> {
    let root = prepare_root(target)?;
    let manifest_path = root.join(PROJECT_MANIFEST);
    let main_path = root.join("src").join("Main.mmt");
    if manifest_path.exists() {
        return Err(format!(
            "project.marmot already exists at {}",
            manifest_path.display()
        ));
    }
    if main_path.exists() {
        return Err(format!(
            "Main module already exists at {}",
            main_path.display()
        ));
    }

    for directory in ["src", "packages", "test"] {
        create_directory(&root.join(directory))?;
    }

    let name = name
        .filter(|name| !name.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| derived_name(&root, "MarmotProject"));
    write(
        &manifest_path,
        &format!(
            "[project]\nname = \"{}\"\nentry = \"src/Main.mmt\"\nsource_dir = \"src\"\npackages_dir = \"packages\"\nprelude_dir = \"MarmotPrelude\"\n\n[test]\ndir = \"test\"\ntimeout_ms = 30000\n",
            escape_toml(&name)
        ),
    )?;
    write(&main_path, "module Main\n\ndef main = fn() -> Int => 0;\n")?;
    // `marmot run` builds into target/.
    let gitignore = root.join(".gitignore");
    if !gitignore.exists() {
        write(&gitignore, "/target/\n")?;
    }
    Ok(root)
}

/// `package.marmot` and a module named after the package.
pub fn package(target: Option<&Path>, name: Option<&str>) -> Result<PathBuf, String> {
    let root = prepare_root(target)?;
    let manifest_path = root.join(PACKAGE_MANIFEST);
    if manifest_path.exists() {
        return Err(format!(
            "package.marmot already exists at {}",
            manifest_path.display()
        ));
    }

    let name = name
        .filter(|name| !name.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| derived_name(&root, "MarmotPackage"));
    let module = module_name(&name);
    let module_file = format!("{module}.mmt");
    let module_path = root.join(&module_file);
    if module_path.exists() {
        return Err(format!(
            "Package module already exists at {}",
            module_path.display()
        ));
    }

    write(
        &manifest_path,
        &format!(
            "[package]\nname = \"{}\"\nversion = \"0.1.0\"\nauthors = []\ndescription = \"\"\nlicense = \"MIT\"\nmarmot_version = \">=1.0.0\"\n\n[package.modules]\nmain = \"{}\"\nexports = [\"{}\"]\n",
            escape_toml(&name),
            escape_toml(&module_file),
            escape_toml(&module)
        ),
    )?;
    write(
        &module_path,
        &format!(
            "module {module}\npublic export {{ Hello }}\n\ndef Hello = fn() -> Text => \"Hello from {module}\";\n"
        ),
    )?;
    Ok(root)
}

#[cfg(test)]
mod tests {
    use super::module_name;

    #[test]
    fn module_names_are_identifiers() {
        assert_eq!(module_name("Greeter"), "Greeter");
        assert_eq!(module_name("123 demo"), "Package123_demo");
        assert_eq!(module_name("my-lib.v2"), "my_lib_v2");
        assert_eq!(module_name("_x"), "_x");
        assert_eq!(module_name(""), "Package");
        assert_eq!(module_name("é"), "__");
    }
}
