mod checksum;
mod cli;
mod execution;
mod paths;
mod project;

#[cfg(test)]
mod test_support;

use cli::{CommandKind, USAGE, parse_options};
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(first) = args.first() else {
        print!("{USAGE}");
        return ExitCode::from(2);
    };

    match first.as_str() {
        "-h" | "--help" | "help" => {
            print!("{USAGE}");
            return ExitCode::SUCCESS;
        }
        "-V" | "--version" => {
            println!("marmot {}", env!("CARGO_PKG_VERSION"));
            return ExitCode::SUCCESS;
        }
        _ => {}
    }

    let Some(kind) = CommandKind::parse(first) else {
        eprintln!("marmot: unknown command '{first}'\n\n{USAGE}");
        return ExitCode::from(2);
    };

    let result = parse_options(kind, &args[1..]).and_then(|options| cli::execute(kind, options));
    result.unwrap_or_else(|error| {
        eprintln!("marmot: error: {error}");
        ExitCode::FAILURE
    })
}
