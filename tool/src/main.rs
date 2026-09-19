mod lockfile;
mod manifest;
mod paths;
mod plan;

#[cfg(test)]
mod tests;

use manifest::Workspace;
use plan::Resolver;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const USAGE: &str = "\
Usage: marmot <command> [file] [options]

Resolves a Marmot project and runs the compiler on it through a build plan.

Commands:
  run [file]      Compile and run the program
  check [file]    Type-check without running
  build [file]    Compile to a .mmc artifact
  plan [file]     Print the build plan instead of compiling

Without [file], the entry named by the nearest project.marmot or
package.marmot is used.

Options:
  --format json       Machine-readable output (run, check, build)
  --embed-sources     Embed sources in the artifact (build)
  -o, --output FILE   Write the plan to FILE (plan)
  --marmotc PATH      The compiler to run; otherwise MARMOTC, then marmotc or
                      Marmot next to this program, then Marmot on PATH
  -h, --help          Show this help
  -V, --version       Show the version
";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CommandKind {
    Run,
    Check,
    Build,
    Plan,
}

impl CommandKind {
    fn parse(name: &str) -> Option<CommandKind> {
        match name {
            "run" => Some(CommandKind::Run),
            "check" => Some(CommandKind::Check),
            "build" => Some(CommandKind::Build),
            "plan" => Some(CommandKind::Plan),
            _ => None,
        }
    }

    fn name(self) -> &'static str {
        match self {
            CommandKind::Run => "run",
            CommandKind::Check => "check",
            CommandKind::Build => "build",
            CommandKind::Plan => "plan",
        }
    }
}

#[derive(Debug, Default)]
struct Options {
    file: Option<PathBuf>,
    json: bool,
    embed_sources: bool,
    output: Option<PathBuf>,
    marmotc: Option<PathBuf>,
}

fn parse_options(kind: CommandKind, args: &[String]) -> Result<Options, String> {
    let mut options = Options::default();
    let mut index = 0;
    while index < args.len() {
        let arg = args[index].as_str();
        let mut value = || {
            index += 1;
            args.get(index)
                .cloned()
                .ok_or_else(|| format!("missing value for {arg}"))
        };
        match arg {
            "--format" => {
                let format = value()?;
                if format != "json" {
                    return Err(format!("unsupported --format '{format}'; expected 'json'"));
                }
                options.json = true;
            }
            "--embed-sources" if kind == CommandKind::Build => options.embed_sources = true,
            "-o" | "--output" if kind == CommandKind::Plan => {
                options.output = Some(PathBuf::from(value()?))
            }
            "--marmotc" => options.marmotc = Some(PathBuf::from(value()?)),
            _ if arg.starts_with('-') => {
                return Err(format!("unknown option for {}: {arg}", kind.name()));
            }
            _ if options.file.is_some() => return Err(format!("{} takes one file", kind.name())),
            _ => options.file = Some(PathBuf::from(arg)),
        }
        index += 1;
    }
    Ok(options)
}

fn find_compiler(explicit: Option<&Path>) -> PathBuf {
    if let Some(path) = explicit {
        return path.to_path_buf();
    }
    if let Some(path) = std::env::var_os("MARMOTC").filter(|value| !value.is_empty()) {
        return PathBuf::from(path);
    }

    let suffix = std::env::consts::EXE_SUFFIX;
    let siblings = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .into_iter()
        .flat_map(|directory| {
            [format!("marmotc{suffix}"), format!("Marmot{suffix}")].map(|name| directory.join(name))
        });
    let current = std::env::current_exe()
        .ok()
        .map(|exe| paths::identity_key(&exe));
    for candidate in siblings {
        if candidate.is_file() && Some(paths::identity_key(&candidate)) != current {
            return candidate;
        }
    }

    PathBuf::from("Marmot")
}

struct CompilerInstall {
    compiler: PathBuf,
}

impl Resolver for CompilerInstall {
    fn resolve(&self, workspace: &Workspace, reason: &str) -> Result<(), String> {
        eprintln!("marmot: resolving packages ({reason})");
        // stdout belongs to the command the user ran (a plan, a program's output).
        let status = Command::new(&self.compiler)
            .arg("install")
            .current_dir(&workspace.root)
            .stdout(std::io::stderr())
            .status()
            .map_err(|error| format!("cannot run {}: {error}", self.compiler.display()))?;
        if status.success() {
            Ok(())
        } else {
            Err(format!(
                "package resolution failed ({} install exited with {status})",
                self.compiler.display()
            ))
        }
    }
}

fn default_entry() -> Result<PathBuf, String> {
    let current = std::env::current_dir()
        .map_err(|error| format!("cannot read the current directory: {error}"))?;
    let workspace = manifest::find_workspace(&current)?
        .ok_or_else(|| "no file given and no project.marmot or package.marmot found".to_string())?;
    workspace.entry_path().ok_or_else(|| {
        format!(
            "no file given and {} names no entry",
            workspace.manifest_path.display()
        )
    })
}

struct TemporaryFile(PathBuf);

impl Drop for TemporaryFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

fn execute(kind: CommandKind, options: Options) -> Result<ExitCode, String> {
    let compiler = find_compiler(options.marmotc.as_deref());
    let entry = match options.file {
        Some(file) => file,
        None => default_entry()?,
    };
    let plan = plan::make_plan(
        &entry,
        &paths::marmot_path(),
        &CompilerInstall {
            compiler: compiler.clone(),
        },
    )?;

    if kind == CommandKind::Plan {
        return match options.output {
            Some(output) => std::fs::write(&output, plan.to_json())
                .map(|()| ExitCode::SUCCESS)
                .map_err(|error| format!("cannot write {}: {error}", output.display())),
            None => {
                print!("{}", plan.to_json());
                Ok(ExitCode::SUCCESS)
            }
        };
    }

    let plan_file = TemporaryFile(
        std::env::temp_dir().join(format!("marmot-plan-{}.json", std::process::id())),
    );
    std::fs::write(&plan_file.0, plan.to_json())
        .map_err(|error| format!("cannot write {}: {error}", plan_file.0.display()))?;

    let mut command = Command::new(&compiler);
    command.arg(kind.name()).arg("--plan").arg(&plan_file.0);
    if options.json {
        command.args(["--format", "json"]);
    }
    if options.embed_sources {
        command.arg("--embed-sources");
    }

    let status = command
        .status()
        .map_err(|error| format!("cannot run {}: {error}", compiler.display()))?;
    Ok(status
        .code()
        .map(|code| ExitCode::from((code & 0xFF) as u8))
        .unwrap_or(ExitCode::FAILURE))
}

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

    let result = parse_options(kind, &args[1..]).and_then(|options| execute(kind, options));
    result.unwrap_or_else(|error| {
        eprintln!("marmot: error: {error}");
        ExitCode::FAILURE
    })
}
