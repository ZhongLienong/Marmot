use super::{Project, compiler};
use lsp_server::{Message, Notification, Request, RequestId};
use serde_json::{Value, json};
use std::io::BufReader;
use std::path::Path;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{Receiver, channel};
use std::time::Duration;
use url::Url;

struct LanguageServer {
    child: Child,
    input: ChildStdin,
    output: Receiver<Message>,
}

impl LanguageServer {
    fn start(compiler: &Path, directory: &Path) -> Self {
        let mut child = Command::new(env!("CARGO_BIN_EXE_marmot"))
            .arg("lsp")
            .arg("--marmotc")
            .arg(compiler)
            .current_dir(directory)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let input = child.stdin.take().unwrap();
        let mut stream = BufReader::new(child.stdout.take().unwrap());
        let (send, output) = channel();
        std::thread::spawn(move || {
            while let Some(message) = Message::read(&mut stream).unwrap() {
                if send.send(message).is_err() {
                    break;
                }
            }
        });
        let mut server = Self {
            child,
            input,
            output,
        };
        let capabilities = server.request(1, "initialize", json!({ "capabilities": {} }));
        assert_eq!(
            capabilities["capabilities"]["textDocumentSync"]["change"],
            1
        );
        assert_eq!(capabilities["capabilities"]["positionEncoding"], "utf-16");
        server.notify("initialized", json!({}));
        server
    }

    fn request(&mut self, id: i32, method: &str, params: Value) -> Value {
        let id = RequestId::from(id);
        Message::from(Request::new(id.clone(), method.into(), params))
            .write(&mut self.input)
            .unwrap();
        let Message::Response(response) = self.read() else {
            panic!("expected response to {method}");
        };
        assert_eq!(response.id, id);
        assert!(response.error.is_none(), "{:?}", response.error);
        response.result.unwrap_or(Value::Null)
    }

    fn notify(&mut self, method: &str, params: Value) {
        Message::from(Notification::new(method.into(), params))
            .write(&mut self.input)
            .unwrap();
    }

    fn read(&self) -> Message {
        self.output
            .recv_timeout(Duration::from_secs(15))
            .expect("expected LSP message")
    }

    fn open(&mut self, uri: &str, text: &str) {
        self.notify(
            "textDocument/didOpen",
            json!({
                "textDocument": { "uri": uri, "languageId": "marmot", "version": 1, "text": text }
            }),
        );
    }

    fn change(&mut self, uri: &str, version: i32, text: &str) {
        self.notify(
            "textDocument/didChange",
            json!({
                "textDocument": { "uri": uri, "version": version },
                "contentChanges": [{ "text": text }]
            }),
        );
    }

    fn diagnostics(&self, count: usize) -> Vec<Value> {
        (0..count)
            .map(|_| {
                let Message::Notification(notification) = self.read() else {
                    panic!("expected diagnostics");
                };
                assert_eq!(notification.method, "textDocument/publishDiagnostics");
                notification.params
            })
            .collect()
    }

    fn shutdown(&mut self) {
        self.request(2, "shutdown", Value::Null);
        self.notify("exit", Value::Null);
        assert!(self.child.wait().unwrap().success());
    }
}

impl Drop for LanguageServer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn diagnostics_for<'a>(reports: &'a [Value], uri: &str) -> &'a Value {
    reports
        .iter()
        .find(|report| report["uri"] == uri)
        .unwrap_or_else(|| {
            panic!("missing diagnostics for {uri} in {reports:?}");
        })
}

#[test]
fn lsp_checks_unsaved_imports_and_clears_versioned_utf16_diagnostics() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("lsp");
    let main = "module Main\n\nimport { <Helper> }\n\ndef answer: Int = Helper::value;\n";
    let helper = "module Helper\npublic export { value }\n\ndef value = 1;\n";
    project.write("project.marmot", "[project]\nentry = \"src/Main.mmt\"\n");
    project.write("src/Main.mmt", main);
    project.write("src/Helper.mmt", helper);
    let main_uri = Url::from_file_path(project.path("src/Main.mmt"))
        .unwrap()
        .to_string();
    let helper_uri = Url::from_file_path(project.path("src/Helper.mmt"))
        .unwrap()
        .to_string();

    let mut server = LanguageServer::start(&compiler, &project.0);
    server.open(&main_uri, main);
    server.open(&helper_uri, helper);
    for report in server.diagnostics(2) {
        assert_eq!(report["diagnostics"], json!([]));
    }

    let bad_helper =
        "module Helper\npublic export { value }\n\ndef value = { \"café 😀\"; missing };\n";
    server.change(&helper_uri, 2, bad_helper);
    let reports = server.diagnostics(2);
    let report = diagnostics_for(&reports, &helper_uri);
    assert_eq!(report["version"], 2);
    let diagnostics = report["diagnostics"].as_array().unwrap();
    assert_eq!(diagnostics.len(), 1, "a shared import is reported once");
    let diagnostic = &diagnostics[0];
    assert_eq!(diagnostic["code"], "TypeUndefinedName");
    assert_eq!(
        diagnostic["range"],
        json!({
            "start": { "line": 3, "character": 25 },
            "end": { "line": 3, "character": 32 }
        })
    );

    let changed_type = "module Helper\npublic export { value }\n\ndef value = \"changed\";\n";
    server.change(&helper_uri, 3, changed_type);
    let reports = server.diagnostics(2);
    let importer = diagnostics_for(&reports, &main_uri);
    assert_eq!(importer["version"], 1);
    assert_eq!(importer["diagnostics"][0]["severity"], 1);
    assert_eq!(
        diagnostics_for(&reports, &helper_uri)["diagnostics"],
        json!([])
    );

    server.change(&helper_uri, 4, bad_helper);
    server.change(&helper_uri, 5, helper);
    let reports = server.diagnostics(2);
    assert_eq!(diagnostics_for(&reports, &helper_uri)["version"], 5);
    assert_eq!(
        diagnostics_for(&reports, &main_uri)["diagnostics"],
        json!([])
    );
    assert_eq!(
        diagnostics_for(&reports, &helper_uri)["diagnostics"],
        json!([])
    );
    assert_eq!(project.read("src/Helper.mmt"), helper);
    assert!(!project.path("marmot.lock").exists());
    server.shutdown();
}
