use super::{Options, print_warnings};
use crate::execution::temporary::{TemporaryDirectory, TemporaryFile};
use crate::execution::{plan, test, toolchain};
use crate::paths;
use crate::project::manifest;
use crate::project::version::Version;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

pub(super) fn execute(
    options: &Options,
    compiler: &Path,
    version: &Version,
) -> Result<ExitCode, String> {
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
    let plan_file = TemporaryFile::new(
        std::env::temp_dir().join(format!("marmot-plan-{}.json", std::process::id())),
    );
    std::fs::write(plan_file.path(), plan.to_json())
        .map_err(|error| format!("cannot write {}: {error}", plan_file.path().display()))?;

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
            _temporary = Some(TemporaryDirectory::new(directory.clone()));
            directory
        }
    };

    let vm = toolchain::find_vm(options.marmotvm.as_deref(), compiler);
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
        plan_file: plan_file.path(),
        compiler,
        vm: &vm,
        jobs: options.jobs,
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
