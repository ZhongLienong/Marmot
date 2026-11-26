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
        assert_eq!(capabilities["capabilities"]["definitionProvider"], true);
        assert_eq!(capabilities["capabilities"]["hoverProvider"], true);
        server.notify("initialized", json!({}));
        server
    }

    fn request(&mut self, id: i32, method: &str, params: Value) -> Value {
        let id = RequestId::from(id);
        Message::from(Request::new(id.clone(), method.into(), params))
            .write(&mut self.input)
            .unwrap();
        let response = loop {
            match self.read() {
                Message::Response(response) => break response,
                Message::Notification(notification)
                    if notification.method == "textDocument/publishDiagnostics" => {}
                message => panic!("expected response to {method}, received {message:?}"),
            }
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

#[test]
fn lsp_definitions_follow_unsaved_imports_and_local_bindings() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("lsp-definitions");
    let main = concat!(
        "module Main\n",
        "import { <Helper>, <Facade> }\n",
        "use Helper.{value}\n",
        "def answer = (\"caf\u{e9} \u{1f600}\", Helper::value);\n",
        "def from_use = value;\n",
        "def reexported = Facade::value;\n",
        "def result = {\n",
        "    def item = 1;\n",
        "    def nested = {\n",
        "        def item = 2;\n",
        "        def read = fn() -> Int => item;\n",
        "        read()\n",
        "    };\n",
        "    (item, nested)\n",
        "};\n",
    );
    let helper = "module Helper\npublic export { value }\n\ndef value = 1;\n";
    let moved_helper =
        "module Helper\npublic export { value }\n\n\ndef _ = \"x\u{1f600}\"; def value = 2;\n";
    let facade = "module Facade\npublic export { value }\nimport { <Helper> }\n";
    project.write("project.marmot", "[project]\nentry = \"src/Main.mmt\"\n");
    project.write("src/Main.mmt", main);
    project.write("src/Helper.mmt", helper);
    project.write("src/Facade.mmt", facade);
    let main_uri = Url::from_file_path(project.path("src/Main.mmt"))
        .unwrap()
        .to_string();
    let helper_path = project.path("src/Helper.mmt");
    let helper_uri = Url::from_file_path(&helper_path).unwrap().to_string();

    let mut server = LanguageServer::start(&compiler, &project.0);
    server.open(&main_uri, main);
    let mut definition = |id, line, character| {
        server.request(
            id,
            "textDocument/definition",
            json!({
                "textDocument": { "uri": main_uri },
                "position": { "line": line, "character": character }
            }),
        )
    };
    let location = |uri: &str, line, start, end| {
        json!({
            "uri": uri,
            "range": {
                "start": { "line": line, "character": start },
                "end": { "line": line, "character": end }
            }
        })
    };
    // Closed imports use the compiler's canonical path, which can expand Windows short names.
    let disk_helper_uri = Url::from_file_path(helper_path.canonicalize().unwrap()).unwrap();
    let original = location(disk_helper_uri.as_str(), 3, 4, 9);
    assert_eq!(definition(3, 3, 34), original);
    assert_eq!(definition(4, 4, 16), original);
    assert_eq!(definition(5, 5, 27), original);
    assert_eq!(definition(6, 10, 35), location(&main_uri, 9, 12, 16));
    assert_eq!(definition(7, 13, 6), location(&main_uri, 7, 8, 12));
    assert_eq!(definition(8, 6, 0), Value::Null);

    server.open(&helper_uri, helper);
    server.change(&helper_uri, 2, moved_helper);
    let result = server.request(
        9,
        "textDocument/definition",
        json!({
            "textDocument": { "uri": main_uri },
            "position": { "line": 3, "character": 34 }
        }),
    );
    assert_eq!(result, location(&helper_uri, 4, 19, 24));
    assert_eq!(project.read("src/Helper.mmt"), helper);
    server.shutdown();
}

#[test]
fn lsp_hover_shows_checked_types_and_declaration_comments() {
    let Some(compiler) = compiler() else { return };
    let project = Project::new("lsp-hover");
    let main = concat!(
        "module Main\n",
        "import { <Helper>, <Facade> }\n",
        "use Helper.{value}\n",
        "def result = (\"\u{1f600}\", Facade::value, Helper::Echo(value));\n",
        "def local_result = {\n",
        "    /// A local binding.\n",
        "    def value = [item for item in 0..2];\n",
        "    value\n",
        "};\n",
        "/// A detached comment.\n",
        "\n",
        "def (first, second) = (true, \"text\");\n",
        "def sample = \"A multiline string\n",
        "/// This is part of the string\";\n",
        "/// Actual documentation.\n",
        "def documented = 7;\n",
    );
    let helper = concat!(
        "module Helper\n",
        "public export { value, Echo }\n",
        "/// The original value.\n",
        "///\n",
        "/// A second paragraph.\n",
        "def value = 7;\n",
        "// An implementation comment.\n",
        "def Echo = fn<Item>(input: Item) -> Item => input;\n",
    );
    let facade = "module Facade\npublic export { value }\nimport { <Helper> }\n";
    project.write("project.marmot", "[project]\nentry = \"src/Main.mmt\"\n");
    project.write("src/Main.mmt", main);
    project.write("src/Helper.mmt", helper);
    project.write("src/Facade.mmt", facade);
    let main_uri = Url::from_file_path(project.path("src/Main.mmt"))
        .unwrap()
        .to_string();
    let helper_uri = Url::from_file_path(project.path("src/Helper.mmt"))
        .unwrap()
        .to_string();

    let mut server = LanguageServer::start(&compiler, &project.0);
    server.open(&main_uri, main);
    let hover = |server: &mut LanguageServer, uri: &str, line: u32, character: usize| {
        server.request(
            3,
            "textDocument/hover",
            json!({
                "textDocument": { "uri": uri },
                "position": { "line": line, "character": character }
            }),
        )
    };
    let imported = main
        .lines()
        .nth(3)
        .unwrap()
        .split_once("Facade::value")
        .unwrap()
        .0;
    let start = imported.encode_utf16().count();
    assert_eq!(
        hover(&mut server, &main_uri, 3, start + 9),
        json!({
            "contents": {
                "kind": "plaintext",
                "value": "Int\n\nThe original value.\n\nA second paragraph."
            },
            "range": {
                "start": { "line": 3, "character": start },
                "end": { "line": 3, "character": start + "Facade::value".len() }
            }
        })
    );
    assert_eq!(
        hover(
            &mut server,
            &main_uri,
            3,
            start + "Facade::value, Helper::".len()
        )["contents"]["value"],
        "transferable fn(Int) -> Int"
    );
    for (line, character, expected) in [
        (6, 9, "Array<Int>\n\nA local binding."),
        (7, 4, "Array<Int>\n\nA local binding."),
        (6, 26, "Int"),
        (11, 5, "Bool"),
        (11, 12, "Text"),
        (15, 4, "Int\n\nActual documentation."),
    ] {
        assert_eq!(
            hover(&mut server, &main_uri, line, character)["contents"]["value"],
            expected
        );
    }
    assert_eq!(hover(&mut server, &main_uri, 5, 8), Value::Null);

    server.open(&helper_uri, helper);
    assert_eq!(
        hover(&mut server, &helper_uri, 7, 4)["contents"]["value"],
        "transferable fn(Item) -> Item"
    );
    assert_eq!(
        hover(&mut server, &helper_uri, 7, 20)["contents"]["value"],
        "Item"
    );
    let unsaved = helper
        .replace("The original value.", "The edited value.")
        .replace("def value = 7;", "def value = \"changed\";");
    server.change(&helper_uri, 2, &unsaved);
    assert_eq!(
        hover(&mut server, &main_uri, 3, start + 9)["contents"]["value"],
        "Text\n\nThe edited value.\n\nA second paragraph."
    );
    assert_eq!(project.read("src/Helper.mmt"), helper);
    server.shutdown();
}
