mod cache;
pub(crate) mod plan;
pub(crate) mod run;
pub(crate) mod standalone;
pub(crate) mod temporary;
pub(crate) mod test;
pub(crate) mod toolchain;

/// Whether what marmotc prints on stderr should keep its colour when this tool
/// passes it on: stderr is a terminal and NO_COLOR is not set. marmotc only
/// colours a terminal of its own, so a captured build is told to with
/// CLICOLOR_FORCE.
pub(crate) fn stderr_colored() -> bool {
    use std::io::IsTerminal;
    std::env::var_os("NO_COLOR").is_none_or(|value| value.is_empty())
        && std::io::stderr().is_terminal()
}

pub(crate) fn strip_ansi(text: &str) -> String {
    let mut stripped = String::with_capacity(text.len());
    let mut in_escape = false;
    for character in text.chars() {
        if character == '\x1b' {
            in_escape = true;
        } else if in_escape {
            in_escape = character != 'm';
        } else {
            stripped.push(character);
        }
    }
    stripped
}
