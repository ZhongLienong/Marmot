use super::{Project, compiler, greeter_project, marmot, succeeded, text, vm};
use std::path::Path;
use std::process::{Command, Output};

fn executable_name(stem: &str) -> String {
    format!("{stem}{}", std::env::consts::EXE_SUFFIX)
}

fn launch(program: &Path, directory: &Path, args: &[&str]) -> Output {
    Command::new(program)
        .args(args)
        .current_dir(directory)
        .env_remove("MARMOTC")
        .env_remove("MARMOTVM")
        .env_remove("MARMOT_PATH")
        .env_remove("MARMOT_LIBRARY_PATH")
        .env("PATH", "")
        .output()
        .unwrap()
}

#[test]
fn only_the_executable_is_needed_after_deployment() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("standalone");
    let output = format!("dist/{}", executable_name("独立 app"));
    let built = marmot(
        &compiler,
        &project.0,
        &["build", "--executable", "-o", &output, "--format", "json"],
    );
    let payload: serde_json::Value = serde_json::from_slice(&succeeded(&built).stdout).unwrap();
    assert_eq!(payload["artifact"]["kind"], "executable");
    assert_eq!(
        payload["artifact"]["nativeLibraries"],
        serde_json::json!([])
    );
    assert_eq!(
        Path::new(payload["artifact"]["path"].as_str().unwrap()),
        project.path(&output)
    );

    let deployed = Project::new("standalone-deployed");
    let executable = deployed.path(&executable_name("独立 app"));
    std::fs::copy(project.path(&output), &executable).unwrap();
    drop(project);

    for args in [&[][..], &["--help"][..], &["disassemble", "ignored"][..]] {
        let ran = launch(&executable, &deployed.0, args);
        assert_eq!(text(&succeeded(&ran).stdout), "hello, plan!\n");
    }
    let version = Command::new(vm(&compiler))
        .arg("--version")
        .output()
        .unwrap();
    assert!(text(&succeeded(&version).stdout).starts_with("marmotvm "));
}

#[test]
fn standalone_defaults_to_the_entry_name_and_preserves_program_exit_codes() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("standalone-exit");
    project.write(
        "Exit.mmt",
        "module ExitProgram\nimport { \"<System>\" }\nSystem::Exit(7);\n",
    );
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["build", "Exit.mmt", "--executable"],
    ));
    let program = project.path(&executable_name("Exit"));
    std::fs::remove_file(project.path("Exit.mmt")).unwrap();
    let ran = launch(&program, &project.0, &[]);
    assert_eq!(ran.status.code(), Some(7));
}

#[test]
fn a_vm_selected_through_path_is_the_binary_that_gets_bundled() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("standalone-path");
    let tools = project.path("tools");
    std::fs::create_dir(&tools).unwrap();
    std::fs::copy(vm(&compiler), tools.join(executable_name("marmotvm"))).unwrap();
    let output = executable_name("app");
    let built = Command::new(env!("CARGO_BIN_EXE_marmot"))
        .args(["build", "--executable", "-o", &output])
        .current_dir(&project.0)
        .env("MARMOTC", &compiler)
        .env("MARMOTVM", "marmotvm")
        .env("MARMOT_PATH", super::prelude())
        .env("PATH", &tools)
        .output()
        .unwrap();
    succeeded(&built);
    std::fs::remove_dir_all(&tools).unwrap();
    let ran = launch(&project.path(&output), &project.0, &[]);
    assert_eq!(text(&succeeded(&ran).stdout), "hello, plan!\n");
}

#[test]
fn standalone_runtime_errors_keep_their_source_after_it_is_deleted() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("standalone-error");
    project.write(
        "Bad.mmt",
        "module Bad\nimport { \"<IO>\" }\nIO::PrintLine((\"bad\" as Int) as Text);\n",
    );
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["build", "Bad.mmt", "--executable"],
    ));
    let expected = marmot(&compiler, &project.0, &["run", "Bad.mmt"]);
    assert!(!expected.status.success());
    std::fs::remove_file(project.path("Bad.mmt")).unwrap();
    let ran = launch(&project.path(&executable_name("Bad")), &project.0, &[]);
    assert_eq!(ran.status.code(), expected.status.code());
    assert_eq!(ran.stdout, expected.stdout);
    assert!(text(&ran.stdout).contains("IO::PrintLine"));
}

#[test]
fn a_failed_build_leaves_the_existing_executable_in_place() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("standalone-build-error");
    let output = executable_name("app");
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["build", "--executable", "-o", &output],
    ));
    let original = std::fs::read(project.path(&output)).unwrap();
    project.write("src/Main.mmt", "module Main\ndef broken = ;\n");
    let failed = marmot(
        &compiler,
        &project.0,
        &["build", "--executable", "-o", &output, "--format", "json"],
    );
    assert!(!failed.status.success());
    let payload: serde_json::Value = serde_json::from_slice(&failed.stdout).unwrap();
    assert_eq!(payload["success"], false);
    assert!(!payload["report"]["errors"].as_array().unwrap().is_empty());
    assert!(payload.get("artifact").is_none());
    assert_eq!(std::fs::read(project.path(&output)).unwrap(), original);
}

#[test]
fn build_accepts_an_output_path_for_bytecode_too() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("bytecode-output");
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["build", "-o", "dist/app.mmc"],
    ));
    let ran = Command::new(vm(&compiler))
        .arg("run")
        .arg(project.path("dist/app.mmc"))
        .output()
        .unwrap();
    assert_eq!(text(&succeeded(&ran).stdout), "hello, plan!\n");
}

#[test]
fn native_libraries_are_bundled_and_extracted_for_each_process() {
    let Some(compiler) = compiler() else { return };
    let library = compiler.with_file_name(if cfg!(windows) {
        "marmot_test_native.dll"
    } else {
        "libmarmot_test_native.so"
    });
    assert!(
        library.is_file(),
        "test library missing: {}",
        library.display()
    );
    let project = Project::new("standalone-native");
    project.write(
        "package.marmot",
        "[package]\nname = \"Native\"\nversion = \"1.0.0\"\n\n[package.modules]\nmain = \"Main.mmt\"\n\n[ffi]\nenabled = true\nlibrary_name = \"marmot_test_native\"\n\n[prebuilt.windows_x64]\npath = \"lib/prebuilt.bin\"\n\n[prebuilt.linux_x86_64]\npath = \"lib/prebuilt.bin\"\n",
    );
    project.write(
        "Main.mmt",
        "module Main\nimport { \"<IO>\" }\nforeign \"marmot_test_answer\" Answer : fn() -> Int from \"marmot_test_native\";\nIO::PrintLine(Answer() as Text);\n",
    );
    std::fs::create_dir_all(project.path("lib")).unwrap();
    std::fs::copy(library, project.path("lib/prebuilt.bin")).unwrap();
    let output = executable_name("native");
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["build", "--executable", "-o", &output],
    ));

    let deployed = Project::new("standalone-native-deployed");
    let executable = deployed.path(&output);
    std::fs::copy(project.path(&output), &executable).unwrap();
    drop(project);
    let temporary = deployed.path("temporary");
    std::fs::create_dir(&temporary).unwrap();
    let processes: Vec<_> = (0..2)
        .map(|_| {
            Command::new(&executable)
                .current_dir(&deployed.0)
                .env("PATH", "")
                .env("TMP", &temporary)
                .env("TEMP", &temporary)
                .env("TMPDIR", &temporary)
                .env_remove("MARMOTC")
                .env_remove("MARMOTVM")
                .env_remove("MARMOT_PATH")
                .env_remove("MARMOT_LIBRARY_PATH")
                .stdout(std::process::Stdio::piped())
                .stderr(std::process::Stdio::piped())
                .spawn()
                .unwrap()
        })
        .collect();
    for process in processes {
        let ran = process.wait_with_output().unwrap();
        assert_eq!(text(&succeeded(&ran).stdout), "42\n");
    }
    assert_eq!(std::fs::read_dir(&temporary).unwrap().count(), 0);
}

#[test]
fn an_unused_native_package_does_not_need_its_library() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("standalone-unused-native");
    project.write(
        "registry/Greeter/package.marmot",
        "[package]\nname = \"Greeter\"\nversion = \"1.2.0\"\n\n[dependencies]\nPunctuation = \"^1.0.0\"\n\n[ffi]\nenabled = true\nlibrary_name = \"missing_unused_library\"\n",
    );
    let output = executable_name("app");
    succeeded(&marmot(
        &compiler,
        &project.0,
        &["build", "--executable", "-o", &output],
    ));
    let ran = launch(&project.path(&output), &project.0, &[]);
    assert_eq!(text(&succeeded(&ran).stdout), "hello, plan!\n");
}

#[test]
fn executable_build_requires_a_runtime_that_supports_the_container() {
    let Some(compiler) = compiler() else { return };
    let project = greeter_project("standalone-old-runtime");
    let failed = marmot(
        &compiler,
        &project.0,
        &[
            "build",
            "--executable",
            "--marmotvm",
            compiler.to_str().unwrap(),
        ],
    );
    assert!(!failed.status.success());
    assert!(text(&failed.stderr).contains("does not support standalone format"));
    assert!(!project.path(&executable_name("src/Main")).exists());
    let failed_json = marmot(
        &compiler,
        &project.0,
        &[
            "build",
            "--executable",
            "--marmotvm",
            compiler.to_str().unwrap(),
            "--format",
            "json",
        ],
    );
    assert!(!failed_json.status.success());
    let payload: serde_json::Value = serde_json::from_slice(&failed_json.stdout).unwrap();
    assert_eq!(payload["success"], false);
    assert!(
        payload["report"]["errors"][0]["message"]
            .as_str()
            .unwrap()
            .contains("does not support standalone format")
    );
}
