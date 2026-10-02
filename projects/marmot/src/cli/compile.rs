use super::{CommandKind, Options, print_warnings};
use crate::execution::temporary::TemporaryFile;
use crate::execution::{plan, run, toolchain};
use crate::paths;
use crate::project::manifest::current_workspace;
use crate::project::version::Version;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

pub(super) fn execute(
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

    let plan_file = TemporaryFile::new(
        std::env::temp_dir().join(format!("marmot-plan-{}.json", std::process::id())),
    );
    std::fs::write(plan_file.path(), plan.to_json())
        .map_err(|error| format!("cannot write {}: {error}", plan_file.path().display()))?;

    if kind == CommandKind::Run {
        let vm = toolchain::find_vm(options.marmotvm.as_deref(), compiler);
        return run::run(&run::RunRequest {
            plan: &plan,
            plan_file: plan_file.path(),
            entry: &entry,
            compiler,
            vm: &vm,
            json: options.json,
            rebuild: options.rebuild,
            jobs: options.jobs,
            timings: options.timings,
        });
    }

    let mut command = Command::new(compiler);
    command.arg(kind.name()).arg("--plan").arg(plan_file.path());
    if options.json {
        command.args(["--format", "json"]);
    }
    if options.embed_sources {
        command.arg("--embed-sources");
    }
    if let Some(jobs) = options.jobs {
        command.arg("--jobs").arg(jobs.to_string());
    }
    if options.timings {
        command.arg("--timings");
    }

    toolchain::run_compiler(command, compiler)
}
