mod checksum;
mod edit;
mod lockfile;
mod manifest;
mod packages;
mod paths;
mod plan;
mod resolver;
mod version;

#[cfg(test)]
mod tests;

use manifest::Workspace;
use packages::Mode;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use version::Version;

const USAGE: &str = "\
Usage: marmot <command> [arguments] [options]

Resolves a Marmot project and runs the compiler on it through a build plan.

Commands:
  run [file]            Compile and run the program
  check [file]          Type-check without running
  build [file]          Compile to a .mmc artifact
  plan [file]           Print the build plan instead of compiling
  install [package]     Resolve and install the project's packages, adding
                        [package] as a dependency first
  update [package]      Resolve every package afresh and report what changed
  remove <package>      Drop a dependency and its installed copies
  list                  Show the resolved dependency tree

Without [file], the entry named by the nearest project.marmot or
package.marmot is used. Package commands act on the manifest found from the
current directory.

Options:
  --format json         Machine-readable output (run, check, build)
  --embed-sources       Embed sources in the artifact (build)
  -o, --output FILE     Write the plan to FILE (plan)
  --version CONSTRAINT  The constraint to record (install <package>); the
                        default is ^ the newest version available
  --marmotc PATH        The compiler to run; otherwise MARMOTC, then marmotc
                        or Marmot next to this program, then Marmot on PATH
  -h, --help            Show this help
  -V                    Show the version
";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CommandKind {
    Run,
    Check,
    Build,
    Plan,
    Install,
    Update,
    Remove,
    List,
}

impl CommandKind {
    fn parse(name: &str) -> Option<CommandKind> {
        match name {
            "run" => Some(CommandKind::Run),
            "check" => Some(CommandKind::Check),
            "build" => Some(CommandKind::Build),
            "plan" => Some(CommandKind::Plan),
            "install" => Some(CommandKind::Install),
            "update" => Some(CommandKind::Update),
            "remove" => Some(CommandKind::Remove),
            "list" => Some(CommandKind::List),
            _ => None,
        }
    }

    fn name(self) -> &'static str {
        match self {
            CommandKind::Run => "run",
            CommandKind::Check => "check",
            CommandKind::Build => "build",
            CommandKind::Plan => "plan",
            CommandKind::Install => "install",
            CommandKind::Update => "update",
            CommandKind::Remove => "remove",
            CommandKind::List => "list",
        }
    }

    fn compiles(self) -> bool {
        matches!(
            self,
            CommandKind::Run | CommandKind::Check | CommandKind::Build | CommandKind::Plan
        )
    }
}

#[derive(Debug, Default)]
struct Options {
    /// A source file, or a package name for the package commands.
    argument: Option<String>,
    json: bool,
    embed_sources: bool,
    output: Option<PathBuf>,
    constraint: Option<String>,
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
            "--format" if kind.compiles() && kind != CommandKind::Plan => {
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
            "--version" if kind == CommandKind::Install => options.constraint = Some(value()?),
            "--marmotc" => options.marmotc = Some(PathBuf::from(value()?)),
            _ if arg.starts_with('-') => {
                return Err(format!("unknown option for {}: {arg}", kind.name()));
            }
            _ if options.argument.is_some() || kind == CommandKind::List => {
                return Err(format!("too many arguments for {}", kind.name()));
            }
            _ => options.argument = Some(arg.to_string()),
        }
        index += 1;
    }

    if kind == CommandKind::Remove && options.argument.is_none() {
        return Err("remove needs the name of a package".to_string());
    }
    if options.constraint.is_some() && options.argument.is_none() {
        return Err("--version needs a package name".to_string());
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
    let current = std::env::current_exe()
        .ok()
        .map(|exe| paths::identity_key(&exe));
    let siblings = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .into_iter()
        .flat_map(|directory| {
            [format!("marmotc{suffix}"), format!("Marmot{suffix}")].map(|name| directory.join(name))
        });
    for candidate in siblings {
        if candidate.is_file() && Some(paths::identity_key(&candidate)) != current {
            return candidate;
        }
    }

    PathBuf::from("Marmot")
}

/// The compiler's version, from `marmotc --version` ("marmot 1.2.3").
/// Packages declare which compiler versions they work with.
fn compiler_version(compiler: &Path) -> Result<Version, String> {
    let output = Command::new(compiler)
        .arg("--version")
        .output()
        .map_err(|error| format!("cannot run {}: {error}", compiler.display()))?;
    let text = String::from_utf8_lossy(&output.stdout);
    text.split_whitespace()
        .nth(1)
        .ok_or_else(|| format!("{} --version printed '{}'", compiler.display(), text.trim()))
        .and_then(|version| {
            Version::parse(version).map_err(|error| {
                format!(
                    "{} --version printed '{}': {error}",
                    compiler.display(),
                    text.trim()
                )
            })
        })
}

fn current_workspace() -> Result<Workspace, String> {
    let current = std::env::current_dir()
        .map_err(|error| format!("cannot read the current directory: {error}"))?;
    manifest::find_workspace(&current)?.ok_or_else(|| {
        "Could not find project.marmot or package.marmot from the current directory.".to_string()
    })
}

fn print_warnings(warnings: &[String]) {
    for warning in warnings {
        eprintln!("[packages] {warning}");
    }
}

struct TemporaryFile(PathBuf);

impl Drop for TemporaryFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

fn compile(
    kind: CommandKind,
    options: &Options,
    compiler: &Path,
    version: &Version,
) -> Result<ExitCode, String> {
    let entry = match &options.argument {
        Some(file) => PathBuf::from(file),
        None => {
            let workspace = current_workspace()?;
            workspace.entry_path().ok_or_else(|| {
                format!(
                    "no file given and {} names no entry",
                    workspace.manifest_path.display()
                )
            })?
        }
    };
    let (plan, warnings) = plan::make_plan(&entry, &paths::marmot_path(), version)?;
    print_warnings(&warnings);

    if kind == CommandKind::Plan {
        return match &options.output {
            Some(output) => std::fs::write(output, plan.to_json())
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

    let mut command = Command::new(compiler);
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

fn install(options: &Options, version: &Version) -> Result<(), String> {
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

fn update(options: &Options, version: &Version) -> Result<(), String> {
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

fn remove(options: &Options, version: &Version) -> Result<(), String> {
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

fn list(version: &Version) -> Result<(), String> {
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

fn execute(kind: CommandKind, options: Options) -> Result<ExitCode, String> {
    let compiler = find_compiler(options.marmotc.as_deref());
    let version = compiler_version(&compiler)?;
    match kind {
        CommandKind::Run | CommandKind::Check | CommandKind::Build | CommandKind::Plan => {
            compile(kind, &options, &compiler, &version)
        }
        CommandKind::Install => install(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::Update => update(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::Remove => remove(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::List => list(&version).map(|()| ExitCode::SUCCESS),
    }
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
