mod cache;
mod checksum;
mod edit;
mod fmt;
mod init;
mod lockfile;
mod manifest;
mod packages;
mod paths;
mod plan;
mod resolver;
mod run;
mod test;
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
  run [file]            Build the program into target/ and run it in marmotvm;
                        an unchanged program is not built again
  check [file]          Type-check without running
  build [file]          Compile to a .mmc artifact
  plan [file]           Print the build plan instead of compiling
  install [package]     Resolve and install the project's packages, adding
                        [package] as a dependency first
  update [package]      Resolve every package afresh and report what changed
  remove <package>      Drop a dependency and its installed copies
  list                  Show the resolved dependency tree
  test [filter]         Build and run the project's tests (the [test]
                        directory) in marmotvm, in parallel
  fmt [file|dir...]     Format sources (options as for marmotc fmt); without
                        a path, write the project's, or --check them
  init [path]           Create a project; with --package, a package

Without [file], the entry named by the nearest project.marmot or
package.marmot is used. Package commands act on the manifest found from the
current directory.

Options:
  --format json         Machine-readable output (run, check, build, test, init)
  --embed-sources       Embed sources in the artifact (build)
  -o, --output FILE     Write the plan to FILE (plan)
  --version CONSTRAINT  The constraint to record (install <package>); the
                        default is ^ the newest version available
  --pattern TEXT        Run tests whose path contains TEXT (test)
  --test FILE           Run one test file (test)
  --package             Create a package instead of a project (init)
  --name NAME           The project or package name (init)
  --marmotc PATH        The compiler to run; otherwise MARMOTC, then marmotc
                        next to this program, then (for a debug build) the one
                        built last in this checkout, then marmotc on PATH
  --rebuild             Build even if the program is unchanged (run)
  --marmotvm PATH       The VM to run programs in (run, test); otherwise MARMOTVM,
                        then marmotvm next to the compiler or this program,
                        then marmotvm on PATH
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
    Test,
    Fmt,
    Init,
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
            "test" => Some(CommandKind::Test),
            "fmt" => Some(CommandKind::Fmt),
            "init" => Some(CommandKind::Init),
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
            CommandKind::Test => "test",
            CommandKind::Fmt => "fmt",
            CommandKind::Init => "init",
        }
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
    marmotvm: Option<PathBuf>,
    pattern: Option<String>,
    test_file: Option<String>,
    package: bool,
    rebuild: bool,
    name: Option<String>,
    /// Everything after the command, for commands passed through as they are.
    raw: Vec<String>,
}

fn parse_options(kind: CommandKind, args: &[String]) -> Result<Options, String> {
    let mut options = Options::default();
    if kind == CommandKind::Fmt {
        let mut index = 0;
        while index < args.len() {
            if args[index] == "--marmotc" {
                options.marmotc = Some(PathBuf::from(
                    args.get(index + 1).ok_or("missing value for --marmotc")?,
                ));
                index += 2;
            } else {
                options.raw.push(args[index].clone());
                index += 1;
            }
        }
        return Ok(options);
    }

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
            "--format"
                if matches!(
                    kind,
                    CommandKind::Run
                        | CommandKind::Check
                        | CommandKind::Build
                        | CommandKind::Test
                        | CommandKind::Init
                ) =>
            {
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
            "--pattern" if kind == CommandKind::Test => options.pattern = Some(value()?),
            "--test" if kind == CommandKind::Test => options.test_file = Some(value()?),
            "--package" if kind == CommandKind::Init => options.package = true,
            "--name" if kind == CommandKind::Init => options.name = Some(value()?),
            "--marmotc" => options.marmotc = Some(PathBuf::from(value()?)),
            "--rebuild" if kind == CommandKind::Run => options.rebuild = true,
            "--marmotvm" if matches!(kind, CommandKind::Run | CommandKind::Test) => {
                options.marmotvm = Some(PathBuf::from(value()?))
            }
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
        .flat_map(|directory| [format!("marmotc{suffix}")].map(|name| directory.join(name)));
    for candidate in siblings {
        if candidate.is_file() && Some(paths::identity_key(&candidate)) != current {
            return candidate;
        }
    }

    checkout_compiler().unwrap_or_else(|| PathBuf::from("marmotc"))
}

/// A debug build of this tool comes from a Marmot checkout, and wants the
/// compiler built there rather than whichever one is installed on PATH. With
/// several presets built, the one built last is the one being worked on:
/// last by either program, since a change to the VM relinks only marmotvm,
/// and the VM run is the one beside the compiler.
fn checkout_compiler() -> Option<PathBuf> {
    if !cfg!(debug_assertions) {
        return None;
    }

    let builds = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()?
        .join("out")
        .join("build")
        .join("ninja");
    newest_build(&builds)
}

/// The marmotc of the preset under `builds` whose marmotc or marmotvm was
/// written last.
fn newest_build(builds: &Path) -> Option<PathBuf> {
    let suffix = std::env::consts::EXE_SUFFIX;
    let modified = |path: PathBuf| path.metadata().ok()?.modified().ok();
    std::fs::read_dir(builds)
        .ok()?
        .filter_map(Result::ok)
        .map(|preset| preset.path().join("out"))
        .filter_map(|directory| {
            let compiler = directory.join(format!("marmotc{suffix}"));
            let compiler_modified = modified(compiler.clone())?;
            let built = modified(directory.join(format!("marmotvm{suffix}")))
                .map_or(compiler_modified, |vm_modified| {
                    vm_modified.max(compiler_modified)
                });
            Some((built, compiler))
        })
        .max_by_key(|(built, _)| *built)
        .map(|(_, compiler)| compiler)
}

/// The compiler's version, from `marmotc --version` ("marmotc 1.2.3").
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

    if kind == CommandKind::Run {
        let vm = run::find_vm(options.marmotvm.as_deref(), compiler);
        return run::run(&run::RunRequest {
            plan: &plan,
            plan_file: &plan_file.0,
            entry: &entry,
            compiler,
            vm: &vm,
            json: options.json,
            rebuild: options.rebuild,
        });
    }

    let mut command = Command::new(compiler);
    command.arg(kind.name()).arg("--plan").arg(&plan_file.0);
    if options.json {
        command.args(["--format", "json"]);
    }
    if options.embed_sources {
        command.arg("--embed-sources");
    }

    run_compiler(command, compiler)
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

fn test(options: &Options, compiler: &Path, version: &Version) -> Result<ExitCode, String> {
    let current = std::env::current_dir()
        .map_err(|error| format!("cannot read the current directory: {error}"))?;
    let workspace = manifest::find_workspace(&current)?;
    let (root, test_directory, timeout_ms) = match &workspace {
        Some(workspace) => (
            workspace.root.clone(),
            workspace.test_path(),
            workspace.test_timeout_ms,
        ),
        None => (current.clone(), current.join("test"), 30_000),
    };

    let (plan, warnings) = plan::inputs_plan(&root, &paths::marmot_path(), version)?;
    print_warnings(&warnings);
    let plan_file = TemporaryFile(
        std::env::temp_dir().join(format!("marmot-plan-{}.json", std::process::id())),
    );
    std::fs::write(&plan_file.0, plan.to_json())
        .map_err(|error| format!("cannot write {}: {error}", plan_file.0.display()))?;

    // In a project the tests are built into target/, mirroring where they are;
    // outside one, somewhere temporary.
    let mut _temporary = None;
    let target_directory = match &workspace {
        Some(workspace) => {
            let relative = test_directory
                .strip_prefix(&workspace.root)
                .map(Path::to_path_buf)
                .unwrap_or_else(|_| PathBuf::from("test"));
            workspace.root.join("target").join(relative)
        }
        None => {
            let directory =
                std::env::temp_dir().join(format!("marmot-test-{}", std::process::id()));
            _temporary = Some(run::TemporaryDirectory::new(directory.clone()));
            directory
        }
    };

    let vm = run::find_vm(options.marmotvm.as_deref(), compiler);
    let timeout = std::time::Duration::from_millis(timeout_ms.max(1) as u64);
    let request = test::TestRequest {
        root: &root,
        test_directory: &test_directory,
        target_directory: &target_directory,
        timeout,
        filter: options.argument.as_deref(),
        pattern: options.pattern.as_deref(),
        test_file: options.test_file.as_deref(),
        plan: &plan,
        plan_file: &plan_file.0,
        compiler,
        vm: &vm,
    };
    let tests = test::discover(
        &test_directory,
        request.filter,
        request.pattern,
        request.test_file,
    );
    let results = test::run_all(&request, &tests);

    if options.json {
        println!("{}", test::json(&root, &test_directory, &results));
    } else {
        print!("{}", test::rendered(&root, timeout, &results));
    }
    Ok(if results.iter().all(|result| result.passed) {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

fn run_compiler(mut command: Command, compiler: &Path) -> Result<ExitCode, String> {
    let status = command
        .status()
        .map_err(|error| format!("cannot run {}: {error}", compiler.display()))?;
    Ok(status
        .code()
        .map(|code| ExitCode::from((code & 0xFF) as u8))
        .unwrap_or(ExitCode::FAILURE))
}

fn init(options: &Options) -> ExitCode {
    let target = options.argument.as_deref().map(Path::new);
    let kind = if options.package {
        "package"
    } else {
        "project"
    };
    let result = if options.package {
        init::package(target, options.name.as_deref())
    } else {
        init::project(target, options.name.as_deref())
    };

    match (result, options.json) {
        (Ok(root), true) => {
            let payload = serde_json::json!({
                "version": 1, "source": "marmot", "command": "init", "success": true,
                "kind": kind, "path": paths::generic(&root),
            });
            println!("{payload}");
            ExitCode::SUCCESS
        }
        (Ok(root), false) => {
            println!("Initialized Marmot {kind} at {}", root.display());
            ExitCode::SUCCESS
        }
        (Err(error), true) => {
            let payload = serde_json::json!({
                "version": 1, "source": "marmot", "command": "init", "success": false,
                "kind": kind, "error": error,
            });
            println!("{payload}");
            ExitCode::FAILURE
        }
        (Err(error), false) => {
            let what = if options.package {
                "Package"
            } else {
                "Project"
            };
            eprintln!("marmot: error: {what} init failed: {error}");
            ExitCode::FAILURE
        }
    }
}

fn execute(kind: CommandKind, options: Options) -> Result<ExitCode, String> {
    if kind == CommandKind::Init {
        return Ok(init(&options));
    }

    let compiler = find_compiler(options.marmotc.as_deref());
    if kind == CommandKind::Fmt {
        let mut command = Command::new(&compiler);
        command.arg("fmt");
        if !fmt::names_a_path(&options.raw) {
            command.args(fmt::project_sources(&current_workspace()?)?);
            if fmt::writes_by_default(&options.raw) {
                command.arg("--write");
            }
        }
        command.args(&options.raw);
        return run_compiler(command, &compiler);
    }

    let version = compiler_version(&compiler)?;
    match kind {
        CommandKind::Run | CommandKind::Check | CommandKind::Build | CommandKind::Plan => {
            compile(kind, &options, &compiler, &version)
        }
        CommandKind::Install => install(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::Update => update(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::Remove => remove(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::List => list(&version).map(|()| ExitCode::SUCCESS),
        CommandKind::Test => test(&options, &compiler, &version),
        CommandKind::Fmt | CommandKind::Init => unreachable!("handled above"),
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
