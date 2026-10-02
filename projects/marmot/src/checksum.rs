use sha2::{Digest, Sha256};
use std::path::Path;

pub(crate) fn render(hash: Sha256) -> String {
    let digest = hash.finalize();
    format!(
        "sha256:{}",
        digest
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>()
    )
}

pub(crate) fn read(path: &Path) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|error| {
        format!(
            "Failed to open file for hashing: {}: {error}",
            path.display()
        )
    })
}

/// `sha256:<hex>` of one file's bytes.
pub(crate) fn file(path: &Path) -> Result<String, String> {
    let mut hash = Sha256::new();
    hash.update(read(path)?);
    Ok(render(hash))
}

/// `sha256:<hex>` of some text.
pub(crate) fn text(value: &str) -> String {
    let mut hash = Sha256::new();
    hash.update(value.as_bytes());
    render(hash)
}
