use std::num::NonZeroUsize;
use std::path::PathBuf;

pub(crate) const USAGE: &str = "\
Usage: marmot <command> [arguments] [options]

Resolves a Marmot project and runs the compiler on it through a build plan.

Commands:
  run [file]            Build the program into target/ and run it in marmotvm;
                        an unchanged program is not built again
  check [file]          Type-check without running
  build [file]          Compile to a .mmc artifact; with --executable, produce
                        a standalone executable for this platform
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
  --executable         Bundle the program and VM into one executable (build)
  -o, --output FILE     Write the artifact or plan to FILE (build, plan)
  --version CONSTRAINT  The constraint to record (install <package>); the
                        default is ^ the newest version available
  --pattern TEXT        Run tests whose path contains TEXT (test)
  --test FILE           Run one test file (test)
  --jobs N              Maximum compiler workers (run, check, build); total
                        worker budget across tests and compilers (test)
  --timings             Print compiler stage times and worker utilization
                        to stderr (run, check, build)
  --package             Create a package instead of a project (init)
  --name NAME           The project or package name (init)
  --marmotc PATH        The compiler to run; otherwise MARMOTC, then marmotc
                        next to this program, then (for a debug build) the one
                        built last in this checkout, then marmotc on PATH
  --rebuild             Build even if the program is unchanged (run)
  --marmotvm PATH       The VM to run or bundle (run, test, build --executable);
                        otherwise MARMOTVM,
                        then the matching checkout build, then marmotvm next
                        to the compiler or this program,
                        then marmotvm on PATH
  -h, --help            Show this help
  -V                    Show the version
";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CommandKind {
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
    pub(crate) fn parse(name: &str) -> Option<CommandKind> {
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

    pub(crate) fn name(self) -> &'static str {
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
pub(crate) struct Options {
    /// A source file, or a package name for the package commands.
    pub(crate) argument: Option<String>,
    pub(crate) json: bool,
    pub(crate) embed_sources: bool,
    pub(crate) executable: bool,
    pub(crate) output: Option<PathBuf>,
    pub(crate) constraint: Option<String>,
    pub(crate) marmotc: Option<PathBuf>,
    pub(crate) marmotvm: Option<PathBuf>,
    pub(crate) pattern: Option<String>,
    pub(crate) test_file: Option<String>,
    pub(crate) package: bool,
    pub(crate) rebuild: bool,
    pub(crate) name: Option<String>,
    pub(crate) jobs: Option<NonZeroUsize>,
    pub(crate) timings: bool,
    /// Everything after the command, for commands passed through as they are.
    pub(crate) raw: Vec<String>,
}

pub(crate) fn parse_options(kind: CommandKind, args: &[String]) -> Result<Options, String> {
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
            "--executable" if kind == CommandKind::Build => options.executable = true,
            "--jobs"
                if matches!(
                    kind,
                    CommandKind::Run | CommandKind::Check | CommandKind::Build | CommandKind::Test
                ) =>
            {
                options.jobs = Some(
                    value()?
                        .parse::<NonZeroUsize>()
                        .map_err(|_| "--jobs requires a positive integer".to_string())?,
                );
            }
            "--timings"
                if matches!(
                    kind,
                    CommandKind::Run | CommandKind::Check | CommandKind::Build
                ) =>
            {
                options.timings = true;
            }
            "-o" | "--output" if matches!(kind, CommandKind::Plan | CommandKind::Build) => {
                options.output = Some(PathBuf::from(value()?))
            }
            "--version" if kind == CommandKind::Install => options.constraint = Some(value()?),
            "--pattern" if kind == CommandKind::Test => options.pattern = Some(value()?),
            "--test" if kind == CommandKind::Test => options.test_file = Some(value()?),
            "--package" if kind == CommandKind::Init => options.package = true,
            "--name" if kind == CommandKind::Init => options.name = Some(value()?),
            "--marmotc" => options.marmotc = Some(PathBuf::from(value()?)),
            "--rebuild" if kind == CommandKind::Run => options.rebuild = true,
            "--marmotvm"
                if matches!(
                    kind,
                    CommandKind::Run | CommandKind::Test | CommandKind::Build
                ) =>
            {
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
    if kind == CommandKind::Build && options.marmotvm.is_some() && !options.executable {
        return Err("--marmotvm requires --executable for build".to_string());
    }
    Ok(options)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn executable_build_options_are_order_independent_and_stay_with_build() {
        let options = parse_options(
            CommandKind::Build,
            &["--marmotvm", "vm", "-o", "app", "--executable"].map(String::from),
        )
        .unwrap();
        assert!(options.executable);
        assert_eq!(options.output, Some(PathBuf::from("app")));
        assert_eq!(options.marmotvm, Some(PathBuf::from("vm")));
        assert!(
            parse_options(CommandKind::Build, &["--marmotvm", "vm"].map(String::from)).is_err()
        );
        for kind in [CommandKind::Run, CommandKind::Check, CommandKind::Plan] {
            assert!(parse_options(kind, &["--executable".into()]).is_err());
        }
    }

    #[test]
    fn compilation_and_tests_accept_positive_worker_budgets() {
        for kind in [
            CommandKind::Run,
            CommandKind::Check,
            CommandKind::Build,
            CommandKind::Test,
        ] {
            let parsed = parse_options(kind, &["--jobs".into(), "4".into()]).unwrap();
            assert_eq!(parsed.jobs.unwrap().get(), 4);
            for value in ["0", "-1", "1.5", "abc", "18446744073709551616"] {
                assert!(parse_options(kind, &["--jobs".into(), value.into()]).is_err());
            }
            assert!(parse_options(kind, &["--jobs".into()]).is_err());
        }
        assert!(parse_options(CommandKind::Plan, &["--jobs".into(), "4".into()]).is_err());
    }
}
