//! `marmot test`: every test file is built by marmotc and run by marmotvm, in
//! parallel, each with the per-test timeout. A test passes when its exit
//! status is what its place says (a test under a `failure` folder must fail)
//! and its output and warnings match the `.expected` and `.warnings.json`
//! snapshots beside it, when there are any.

use crate::paths;
use crate::plan::Plan;
use serde_json::Value;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

const GREEN: &str = "\x1b[92m";
const RED: &str = "\x1b[91m";
const YELLOW: &str = "\x1b[93m";
const BLUE: &str = "\x1b[94m";
const CYAN: &str = "\x1b[96m";
const GRAY: &str = "\x1b[90m";
const BOLD: &str = "\x1b[1m";
const RESET: &str = "\x1b[0m";
const RULE: &str = "============================================================";

/// Files directly in the test directory that are scratch files, not tests.
const SCRATCH_FILES: [&str; 4] = [
    "minimal_test.mmt",
    "test.mmt",
    "simple_test.mmt",
    "test_backup.mmt",
];

pub struct TestRequest<'a> {
    pub root: &'a Path,
    pub test_directory: &'a Path,
    /// Where the built tests go.
    pub target_directory: &'a Path,
    pub timeout: Duration,
    pub filter: Option<&'a str>,
    pub pattern: Option<&'a str>,
    pub test_file: Option<&'a str>,
    pub plan: &'a Plan,
    /// The plan's inputs, without an entry: each test is the entry.
    pub plan_file: &'a Path,
    pub compiler: &'a Path,
    pub vm: &'a Path,
}

#[derive(Debug, Default)]
pub struct TestResult {
    pub name: String,
    pub passed: bool,
    pub expected_to_fail: bool,
    pub timed_out: bool,
    pub exit_code: i32,
    pub duration_ms: u128,
    pub error: Option<String>,
    pub output: String,
    pub warnings: Vec<Value>,
}

/// A test is expected to fail when any folder on its path is named `failure`.
fn expected_to_fail(relative: &Path) -> bool {
    relative
        .parent()
        .into_iter()
        .flat_map(Path::components)
        .any(|part| {
            part.as_os_str()
                .to_string_lossy()
                .eq_ignore_ascii_case("failure")
        })
}

fn matches(relative: &Path, filter: Option<&str>, pattern: Option<&str>) -> bool {
    if let Some(filter) = filter
        && !paths::generic(relative).contains(filter)
    {
        return false;
    }
    if let Some(pattern) = pattern {
        let file_name = relative
            .file_name()
            .map(|name| name.to_string_lossy().to_lowercase())
            .unwrap_or_default();
        if !file_name.contains(&pattern.to_lowercase()) {
            return false;
        }
    }
    true
}

fn walk(directory: &Path, found: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(directory) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            walk(&path, found);
        } else if path.extension().is_some_and(|extension| extension == "mmt") {
            found.push(path);
        }
    }
}

/// The test files, in order: `--test` names one; otherwise every `.mmt` under
/// the test directory that passes the filter and pattern, except the
/// `doc_examples` folder and scratch files at the top.
pub fn discover(
    test_directory: &Path,
    filter: Option<&str>,
    pattern: Option<&str>,
    test_file: Option<&str>,
) -> Vec<PathBuf> {
    if let Some(test_file) = test_file {
        for candidate in [
            test_directory.join(test_file),
            test_directory.join(format!("{test_file}.mmt")),
        ] {
            if candidate.is_file() {
                return vec![candidate];
            }
        }
    }

    let mut found = Vec::new();
    walk(test_directory, &mut found);
    found.sort();
    found.retain(|path| {
        let Ok(relative) = path.strip_prefix(test_directory) else {
            return false;
        };
        let top = relative.components().next().map(|part| part.as_os_str());
        if top.is_some_and(|top| top == "doc_examples") {
            return false;
        }
        if relative.components().count() == 1
            && SCRATCH_FILES
                .iter()
                .any(|name| relative.as_os_str() == *name)
        {
            return false;
        }
        matches(relative, filter, pattern)
    });
    found
}

struct Finished {
    code: Option<i32>,
    output: String,
    timed_out: bool,
}

fn drain(mut stream: impl Read + Send + 'static) -> std::thread::JoinHandle<Vec<u8>> {
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let _ = stream.read_to_end(&mut bytes);
        bytes
    })
}

/// Runs `command` to completion or until `deadline`, when it is killed. The
/// output is stdout followed by stderr.
fn run_until(mut command: Command, deadline: Instant) -> Result<Finished, String> {
    let mut child: Child = command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("cannot run {:?}: {error}", command.get_program()))?;
    let stdout = drain(child.stdout.take().expect("stdout is piped"));
    let stderr = drain(child.stderr.take().expect("stderr is piped"));

    let mut timed_out = false;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) if Instant::now() >= deadline => {
                let _ = child.kill();
                let _ = child.wait();
                timed_out = true;
                break None;
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(5)),
            Err(error) => return Err(format!("cannot wait for a test: {error}")),
        }
    };

    let mut output = stdout.join().unwrap_or_default();
    output.extend(stderr.join().unwrap_or_default());
    Ok(Finished {
        code: status.and_then(|status| status.code()),
        output: String::from_utf8_lossy(&output).into_owned(),
        timed_out,
    })
}

/// The canonical root with a trailing `/`, as snapshots strip it. Its case is
/// kept: it must match the paths the compiler prints.
fn root_prefix(root: &Path) -> String {
    let canonical = std::fs::canonicalize(root).unwrap_or_else(|_| root.to_path_buf());
    let mut prefix = paths::display(&canonical).replace('\\', "/");
    if !prefix.ends_with('/') {
        prefix.push('/');
    }
    prefix
}

fn without_root(text: &str, root_prefix: &str) -> String {
    text.replace('\\', "/").replace(root_prefix, "")
}

fn strip_ansi(text: &str) -> String {
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

/// Output as a snapshot compares it: no colour, no root, `/` separators, `\n`
/// line ends, no trailing blanks on a line, and no final newline. A run of `\r`
/// is one line end, with or without a `\n` after it: a source line read with
/// its `\r` and written through a text-mode stream ends in `\r\r\n`.
fn snapshot_text(text: &str, root_prefix: &str) -> String {
    let stripped = without_root(&strip_ansi(text), root_prefix);
    let mut clean = String::with_capacity(stripped.len());
    let mut after_carriage_return = false;
    for character in stripped.chars() {
        if character == '\r' {
            after_carriage_return = true;
            continue;
        }
        if after_carriage_return && character != '\n' {
            clean.push('\n');
        }
        after_carriage_return = false;
        clean.push(character);
    }
    if after_carriage_return {
        clean.push('\n');
    }
    let lines: Vec<&str> = clean
        .split('\n')
        .map(|line| line.trim_end_matches([' ', '\t']))
        .collect();
    let mut joined = lines.join("\n");
    if joined.ends_with('\n') {
        joined.pop();
    }
    joined
}

fn normalized_warning(mut warning: Value, root_prefix: &str) -> Value {
    for member in ["file", "file_path"] {
        if let Some(text) = warning.get(member).and_then(Value::as_str) {
            warning[member] = Value::from(without_root(text, root_prefix));
        }
    }
    warning
}

/// The compiler's machine-readable warning lines, and everything else.
fn split_warnings(output: &str, root_prefix: &str) -> (Vec<Value>, String) {
    let mut warnings = Vec::new();
    let mut rest = String::with_capacity(output.len());
    for line in output.split_inclusive('\n') {
        match line.strip_prefix("MARMOT_WARNING\t") {
            Some(payload) => {
                if let Ok(warning) = serde_json::from_str::<Value>(payload.trim_end()) {
                    warnings.push(normalized_warning(warning, root_prefix));
                }
            }
            None => rest.push_str(line),
        }
    }
    (warnings, rest)
}

fn read_optional(path: &Path) -> Option<String> {
    std::fs::read_to_string(path).ok()
}

fn run_one(request: &TestRequest, test: &Path) -> TestResult {
    let started = Instant::now();
    let deadline = started + request.timeout;
    let relative = test
        .strip_prefix(request.test_directory)
        .map(Path::to_path_buf)
        .unwrap_or_else(|_| test.file_name().map(PathBuf::from).unwrap_or_default());
    let mut result = TestResult {
        name: paths::generic(&relative),
        expected_to_fail: expected_to_fail(&relative),
        ..TestResult::default()
    };
    let prefix = root_prefix(request.root);
    let program = request
        .target_directory
        .join(relative.with_extension("mmc"));

    let mut build = Command::new(request.compiler);
    build
        .current_dir(request.root)
        .env("MARMOT_TEST_WARNING_FORMAT", "machine")
        .arg("build")
        .arg("--plan")
        .arg(request.plan_file)
        .arg(test)
        .arg("-o")
        .arg(&program)
        .arg("--quiet");
    let built = match run_until(build, deadline) {
        Ok(built) => built,
        Err(error) => {
            result.error = Some(error);
            result.exit_code = 1;
            return result;
        }
    };
    let (warnings, build_output) = split_warnings(&built.output, &prefix);
    result.warnings = warnings;
    result.output = build_output;
    result.timed_out = built.timed_out;
    result.exit_code = built.code.unwrap_or(1);

    if built.code == Some(0) {
        let mut vm = Command::new(request.vm);
        vm.current_dir(request.root).arg(&program);
        for library in &request.plan.native_libraries {
            vm.arg("--library")
                .arg(format!("{}={}", library.name, library.path));
        }
        match run_until(vm, deadline) {
            Ok(ran) => {
                result.output.push_str(&ran.output);
                result.timed_out = ran.timed_out;
                result.exit_code = ran.code.unwrap_or(1);
            }
            Err(error) => {
                result.error = Some(error);
                result.exit_code = 1;
                return result;
            }
        }
    }
    result.duration_ms = started.elapsed().as_millis();

    if result.timed_out {
        result.exit_code = 1;
        result.duration_ms = request.timeout.as_millis();
        result.error = Some(format!(
            "Timed out after {}ms.",
            request.timeout.as_millis()
        ));
        return result;
    }

    let succeeded = result.exit_code == 0;
    result.passed = succeeded != result.expected_to_fail;
    if !result.passed {
        result.error = Some(if result.expected_to_fail {
            format!("Expected a non-zero exit code, got {}.", result.exit_code)
        } else {
            format!("Expected exit code 0, got {}.", result.exit_code)
        });
        return result;
    }

    let expected_output = read_optional(&test.with_extension("expected"));
    let expected_warnings = read_optional(&test.with_extension("warnings.json"));
    let embeds_root = |snapshot: &Option<String>| {
        snapshot
            .as_deref()
            .is_some_and(|text| text.replace('\\', "/").contains(&prefix))
    };
    if embeds_root(&expected_output) || embeds_root(&expected_warnings) {
        result.passed = false;
        result.error = Some(format!(
            "Snapshot contains the absolute project path '{prefix}'. Write paths relative to the project root so the snapshot passes in other checkouts."
        ));
        return result;
    }

    if let Some(expected) = expected_output.filter(|text| !text.is_empty()) {
        let expected = snapshot_text(&expected, &prefix);
        let actual = snapshot_text(&result.output, &prefix);
        if expected != actual {
            result.passed = false;
            result.error = Some(format!(
                "Output snapshot mismatch.\nExpected:\n{expected}\n\nActual:\n{actual}"
            ));
            return result;
        }
    }

    if let Some(expected) = expected_warnings.filter(|text| !text.trim().is_empty()) {
        let expected: Vec<Value> = match serde_json::from_str::<Vec<Value>>(&expected) {
            Ok(warnings) => warnings
                .into_iter()
                .map(|warning| normalized_warning(warning, &prefix))
                .collect(),
            Err(error) => {
                result.passed = false;
                result.error = Some(format!("The warning snapshot is not a JSON array: {error}"));
                return result;
            }
        };
        if expected != result.warnings {
            result.passed = false;
            result.error = Some(format!(
                "Warning snapshot mismatch.\nExpected:\n{}\n\nActual:\n{}",
                serde_json::to_string_pretty(&expected).unwrap_or_default(),
                serde_json::to_string_pretty(&result.warnings).unwrap_or_default()
            ));
        }
    }
    result
}

/// Runs every test, as many at once as the machine has cores, and returns the
/// results in discovery order.
pub fn run_all(request: &TestRequest, tests: &[PathBuf]) -> Vec<TestResult> {
    let workers = std::thread::available_parallelism()
        .map(|count| count.get())
        .unwrap_or(1)
        .min(tests.len().max(1));
    let next = AtomicUsize::new(0);
    let results: Mutex<Vec<(usize, TestResult)>> = Mutex::new(Vec::with_capacity(tests.len()));
    std::thread::scope(|scope| {
        for _ in 0..workers {
            scope.spawn(|| {
                loop {
                    let index = next.fetch_add(1, Ordering::SeqCst);
                    let Some(test) = tests.get(index) else {
                        break;
                    };
                    let result = run_one(request, test);
                    results
                        .lock()
                        .expect("no test thread panics holding the lock")
                        .push((index, result));
                }
            });
        }
    });
    let mut results = results
        .into_inner()
        .expect("the test threads have finished");
    results.sort_by_key(|(index, _)| *index);
    results.into_iter().map(|(_, result)| result).collect()
}

fn status_label(result: &TestResult) -> String {
    if result.timed_out {
        format!("{YELLOW}[TIMEOUT]{RESET}")
    } else if result.passed {
        format!("{GREEN}[OK]{RESET}")
    } else {
        format!("{RED}[FAIL]{RESET}")
    }
}

fn type_label(result: &TestResult) -> String {
    if result.expected_to_fail {
        format!("{YELLOW}[SHOULD-FAIL]{RESET}")
    } else {
        format!("{CYAN}[SUCCESS]{RESET}")
    }
}

pub fn rendered(root: &Path, timeout: Duration, results: &[TestResult]) -> String {
    let passed = results.iter().filter(|result| result.passed).count();
    let timed_out = results.iter().filter(|result| result.timed_out).count();
    let duration: u128 = results.iter().map(|result| result.duration_ms).sum();

    let mut output = format!(
        "{BOLD}Marmot Test Suite{RESET}\n{GRAY}{RULE}{RESET}\nRoot: {CYAN}{}{RESET}\nTests: {CYAN}{}{RESET}\nTimeout: {CYAN}{}ms{RESET}\n{GRAY}{RULE}{RESET}\n",
        root.display(),
        results.len(),
        timeout.as_millis()
    );
    let mut last_category = String::new();
    for result in results {
        let category = result.name.split('/').next().unwrap_or("tests").to_string();
        if category != last_category {
            output.push_str(&format!("\n{BOLD}{BLUE}[{category}]{RESET}\n"));
            last_category = category;
        }
        output.push_str(&format!(
            "{} {} {} {GRAY}({}ms){RESET}\n",
            status_label(result),
            type_label(result),
            result.name,
            result.duration_ms
        ));
        if let (false, Some(error)) = (result.passed, &result.error) {
            output.push_str(&format!("  {RED}{error}{RESET}\n"));
        }
    }

    output.push_str(&format!(
        "\n{GRAY}{RULE}{RESET}\n{BOLD}Test Summary{RESET}\n"
    ));
    output.push_str(&if passed == results.len() {
        format!("{GREEN}{BOLD}[SUCCESS] All tests passed!{RESET}\n")
    } else {
        format!("{RED}{BOLD}[FAILED] Some tests failed{RESET}\n")
    });
    output.push_str(&format!("Total: {passed}/{} passed\n", results.len()));
    if timed_out > 0 {
        output.push_str(&format!("Timed out: {timed_out}\n"));
    }
    output.push_str(&format!("Duration: {duration}ms\n"));
    output
}

pub fn json(root: &Path, test_directory: &Path, results: &[TestResult]) -> Value {
    let passed = results.iter().filter(|result| result.passed).count();
    let entries: Vec<Value> = results
        .iter()
        .map(|result| {
            serde_json::json!({
                "name": result.name,
                "path": result.name,
                "passed": result.passed,
                "expectedToFail": result.expected_to_fail,
                "timedOut": result.timed_out,
                "exitCode": result.exit_code,
                "durationMs": result.duration_ms,
                "error": result.error,
                "output": result.output,
                "warnings": result.warnings,
            })
        })
        .collect();
    serde_json::json!({
        "version": 1,
        "source": "marmot",
        "command": "test",
        "success": passed == results.len(),
        "root": paths::generic(root),
        "testDir": paths::generic(test_directory),
        "summary": {
            "total": results.len(),
            "passed": passed,
            "failed": results.len() - passed,
            "timedOut": results.iter().filter(|result| result.timed_out).count(),
            "durationMs": results.iter().map(|result| result.duration_ms).sum::<u128>(),
        },
        "results": entries,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_test_under_a_failure_folder_is_expected_to_fail() {
        assert!(expected_to_fail(Path::new("ffi/failure/pop.mmt")));
        assert!(expected_to_fail(Path::new("Failure/x.mmt")));
        assert!(!expected_to_fail(Path::new("ffi/success/failure.mmt")));
    }

    #[test]
    fn filter_matches_the_path_and_pattern_the_file_name() {
        let path = Path::new("union/success/Tree.mmt");
        assert!(matches(path, Some("union/"), None));
        assert!(!matches(path, Some("pipe"), None));
        assert!(matches(path, None, Some("tree")));
        assert!(!matches(path, None, Some("union")));
    }

    #[test]
    fn a_snapshot_ignores_colour_the_root_and_trailing_blanks() {
        let prefix = "C:/work/proj/";
        assert_eq!(
            snapshot_text(
                "\x1b[91merror\x1b[0m at C:\\work\\proj\\src\\A.mmt:1  \r\nnext\t\n",
                prefix
            ),
            "error at src/A.mmt:1\nnext"
        );
        assert_eq!(
            snapshot_text("9 | line;\r\r\n  |\r\n", prefix),
            "9 | line;\n  |"
        );
        assert_eq!(snapshot_text("a\rb", prefix), "a\nb");
    }

    #[test]
    fn the_root_prefix_keeps_the_case_the_compiler_prints() {
        let root = std::env::current_dir().unwrap();
        let prefix = root_prefix(&root);
        let printed = format!(
            "{}/src/A.mmt",
            paths::display(&std::fs::canonicalize(&root).unwrap())
        );
        assert_eq!(without_root(&printed, &prefix), "src/A.mmt");
    }

    #[test]
    fn warning_lines_come_out_of_the_output() {
        let (warnings, rest) = split_warnings(
            "warning text\nMARMOT_WARNING\t{\"code\":\"UnusedLocal\",\"file\":\"C:/p/A.mmt\"}\nafter\n",
            "C:/p/",
        );
        assert_eq!(
            warnings,
            vec![serde_json::json!({ "code": "UnusedLocal", "file": "A.mmt" })]
        );
        assert_eq!(rest, "warning text\nafter\n");
    }
}
