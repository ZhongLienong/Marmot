mod analysis;

use crate::project::version::Version;
use analysis::{Definitions, Diagnostics, Document, Documents};
use crossbeam_channel::{after, never, select_biased, unbounded};
use lsp_server::{Connection, ErrorCode, Message, Notification, RequestId, Response};
use serde::Deserialize;
use serde_json::{Value, json};
use std::collections::BTreeSet;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;
use std::time::{Duration, Instant};
use url::Url;

const EDIT_DELAY: Duration = Duration::from_millis(200);

pub(crate) fn run(compiler: PathBuf, version: Version) -> Result<ExitCode, String> {
    let (connection, threads) = Connection::stdio();
    connection
        .initialize(json!({
            "positionEncoding": "utf-16",
            "textDocumentSync": { "openClose": true, "change": 1, "save": true },
            "definitionProvider": true
        }))
        .map_err(|error| error.to_string())?;
    let status = serve(
        &connection,
        Arc::new(analysis::Compiler::new(compiler, version)),
    )?;
    drop(connection);
    threads.join().map_err(|error| error.to_string())?;
    Ok(status)
}

fn serve(connection: &Connection, compiler: Arc<analysis::Compiler>) -> Result<ExitCode, String> {
    let mut documents = Documents::new();
    let mut published = BTreeSet::new();
    let mut generation = 0;
    let mut deadline: Option<Instant> = None;
    let mut running = false;
    let mut definitions: Option<Definitions> = None;
    let mut pending: Vec<DefinitionRequest> = Vec::new();
    let (send_result, results) = unbounded::<(u64, (Diagnostics, Definitions))>();

    loop {
        let timer = match deadline {
            Some(due) if !running => after(due.saturating_duration_since(Instant::now())),
            _ => never(),
        };

        select_biased! {
            recv(connection.receiver) -> message => {
                match message {
                    Ok(Message::Request(request)) => {
                        if connection.handle_shutdown(&request).map_err(|error| error.to_string())? {
                            return Ok(ExitCode::SUCCESS);
                        }
                        if request.method != "textDocument/definition" {
                            respond(connection, Response::new_err(request.id, ErrorCode::MethodNotFound as i32, request.method))?;
                            continue;
                        }
                        let params = match serde_json::from_value::<DefinitionParams>(request.params) {
                            Ok(params) => params,
                            Err(error) => {
                                respond(connection, Response::new_err(request.id, ErrorCode::InvalidParams as i32, error.to_string()))?;
                                continue;
                            }
                        };
                        let Some(document) = documents.get(&params.text_document.uri) else {
                            respond(connection, Response::new_ok(request.id, Value::Null))?;
                            continue;
                        };
                        let query = DefinitionRequest { id: request.id, params, text: Arc::clone(&document.text) };
                        if let Some(definitions) = &mut definitions {
                            respond(connection, query.resolve(&documents, definitions))?;
                        } else {
                            pending.push(query);
                            deadline = Some(Instant::now());
                        }
                    }
                    Ok(Message::Notification(notification)) if notification.method == "exit" => {
                        return Ok(ExitCode::FAILURE);
                    }
                    Ok(Message::Notification(notification)) if notification.method == "$/cancelRequest" => {
                        if let Ok(CancelParams { id }) = serde_json::from_value(notification.params)
                            && let Some(index) = pending.iter().position(|query| query.id == id)
                        {
                            pending.remove(index);
                            respond(connection, Response::new_err(id, ErrorCode::RequestCanceled as i32, "Request canceled".into()))?;
                        }
                    }
                    Ok(Message::Notification(notification)) => {
                        if update_documents(&mut documents, notification) {
                            generation += 1;
                            definitions = None;
                            let delay = if pending.is_empty() { EDIT_DELAY } else { Duration::ZERO };
                            deadline = Some(Instant::now() + delay);
                        }
                    }
                    Ok(Message::Response(_)) => {}
                    Err(_) => return Ok(ExitCode::FAILURE),
                }
            }
            recv(results) -> result => {
                let (checked_generation, (diagnostics, mut checked_definitions)) = result.map_err(|error| error.to_string())?;
                running = false;
                if checked_generation == generation {
                    deadline = None;
                    for query in pending.drain(..) {
                        respond(connection, query.resolve(&documents, &mut checked_definitions))?;
                    }
                    definitions = Some(checked_definitions);
                    publish(connection, &documents, &mut published, diagnostics)?;
                }
            }
            recv(timer) -> _ => {
                deadline = None;
                let snapshot = documents.clone();
                let compiler = Arc::clone(&compiler);
                let send_result = send_result.clone();
                running = true;
                std::thread::spawn(move || {
                    let _ = send_result.send((generation, compiler.analyze(&snapshot)));
                });
            }
        }
    }
}

fn respond(connection: &Connection, response: Response) -> Result<(), String> {
    connection
        .sender
        .send(response.into())
        .map_err(|error| error.to_string())
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct DefinitionParams {
    text_document: TextDocument,
    position: Position,
}

#[derive(Deserialize)]
struct Position {
    line: u32,
    character: usize,
}

#[derive(Deserialize)]
struct CancelParams {
    id: RequestId,
}

struct DefinitionRequest {
    id: RequestId,
    params: DefinitionParams,
    text: Arc<str>,
}

impl DefinitionRequest {
    fn resolve(self, documents: &Documents, definitions: &mut Definitions) -> Response {
        let uri = self.params.text_document.uri;
        if !documents
            .get(&uri)
            .is_some_and(|document| Arc::ptr_eq(&document.text, &self.text))
        {
            return Response::new_err(
                self.id,
                ErrorCode::ContentModified as i32,
                "Document changed".into(),
            );
        }
        let position = self.params.position;
        Response::new_ok(
            self.id,
            definitions.at(&uri, &self.text, position.line, position.character),
        )
    }
}

#[derive(Deserialize)]
struct TextDocument {
    uri: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct DocumentParams<T> {
    text_document: T,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct OpenDocument {
    uri: String,
    language_id: String,
    version: i32,
    text: String,
}

#[derive(Deserialize)]
struct VersionedDocument {
    uri: String,
    version: i32,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct ChangeDocument {
    text_document: VersionedDocument,
    content_changes: Vec<FullText>,
}

#[derive(Deserialize)]
struct FullText {
    text: String,
}

fn update_documents(documents: &mut Documents, notification: Notification) -> bool {
    match notification.method.as_str() {
        "textDocument/didOpen" => {
            let Ok(DocumentParams { text_document }) =
                serde_json::from_value::<DocumentParams<OpenDocument>>(notification.params)
            else {
                return false;
            };
            let Some(path) = Url::parse(&text_document.uri)
                .ok()
                .and_then(|url| url.to_file_path().ok())
            else {
                return false;
            };
            if text_document.language_id != "marmot" || !path.is_file() {
                return false;
            }
            documents.insert(
                text_document.uri,
                Document {
                    path,
                    version: text_document.version,
                    text: text_document.text.into(),
                },
            );
            true
        }
        "textDocument/didChange" => {
            let Ok(change) = serde_json::from_value::<ChangeDocument>(notification.params) else {
                return false;
            };
            let Some(document) = documents.get_mut(&change.text_document.uri) else {
                return false;
            };
            let Some(content) = change.content_changes.into_iter().last() else {
                return false;
            };
            if change.text_document.version <= document.version {
                return false;
            }
            document.version = change.text_document.version;
            document.text = content.text.into();
            true
        }
        "textDocument/didClose" | "textDocument/didSave" => {
            let Ok(DocumentParams { text_document }) =
                serde_json::from_value::<DocumentParams<TextDocument>>(notification.params)
            else {
                return false;
            };
            if notification.method == "textDocument/didClose" {
                documents.remove(&text_document.uri).is_some()
            } else {
                documents.contains_key(&text_document.uri)
            }
        }
        _ => false,
    }
}

fn publish(
    connection: &Connection,
    documents: &Documents,
    published: &mut BTreeSet<String>,
    mut diagnostics: Diagnostics,
) -> Result<(), String> {
    let affected: BTreeSet<_> = published
        .iter()
        .chain(diagnostics.keys())
        .chain(documents.keys())
        .cloned()
        .collect();
    *published = diagnostics.keys().cloned().collect();
    for uri in affected {
        let mut params =
            json!({ "uri": uri, "diagnostics": diagnostics.remove(&uri).unwrap_or_default() });
        if let Some(document) = documents.get(&uri) {
            params["version"] = json!(document.version);
        }
        connection
            .sender
            .send(Notification::new("textDocument/publishDiagnostics".into(), params).into())
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}
