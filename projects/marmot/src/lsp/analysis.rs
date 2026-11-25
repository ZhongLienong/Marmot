use crate::execution::plan;
use crate::paths;
use crate::project::version::Version;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Arc;
use url::Url;

#[derive(Clone)]
pub(super) struct Document {
    pub(super) path: PathBuf,
    pub(super) version: i32,
    pub(super) text: Arc<str>,
}

pub(super) type Documents = BTreeMap<String, Document>;
pub(super) type Diagnostics = BTreeMap<String, Vec<Value>>;

pub(super) struct Definitions {
    entries: BTreeMap<String, Vec<CompilerDefinition>>,
    files: HashMap<String, Source>,
}

impl Definitions {
    pub(super) fn at(&mut self, uri: &str, text: &str, line: u32, character: usize) -> Value {
        let Some(definitions) = self.entries.get(uri) else {
            return Value::Null;
        };
        let Some(column) = byte_column(text, line, character) else {
            return Value::Null;
        };
        let position = (line + 1, column);
        let Some(definition) = definitions.iter().find(|definition| {
            let reference = &definition.reference;
            (reference.line, reference.column) <= position
                && position < (reference.end_line, reference.end_column)
        }) else {
            return Value::Null;
        };
        let declaration = &definition.declaration;
        let Some(source) = source_for(&mut self.files, &declaration.file) else {
            return Value::Null;
        };
        let range = CompilerLocation {
            line: Some(declaration.line),
            column: Some(declaration.column),
            end_line: Some(declaration.end_line),
            end_column: Some(declaration.end_column),
            ..CompilerLocation::default()
        };
        json!({ "uri": source.uri, "range": range.range(&source.text) })
    }
}

pub(super) struct Compiler {
    executable: PathBuf,
    version: Version,
    search_paths: Vec<PathBuf>,
}

impl Compiler {
    pub(super) fn new(executable: PathBuf, version: Version) -> Self {
        Self {
            executable,
            version,
            search_paths: paths::marmot_path(),
        }
    }

    pub(super) fn analyze(&self, documents: &Documents) -> (Diagnostics, Definitions) {
        let mut files = HashMap::new();
        let mut overlays = BTreeMap::new();
        for (uri, document) in documents {
            files.insert(
                paths::identity_key(&document.path),
                Source {
                    uri: uri.clone(),
                    text: Arc::clone(&document.text),
                },
            );
            overlays.insert(paths::display(&document.path), document.text.as_ref());
        }

        let mut result = Diagnostics::new();
        let mut entries = BTreeMap::new();
        for (uri, document) in documents {
            match self.analyze_entry(&document.path, &overlays) {
                Ok(response) => {
                    entries.insert(uri.clone(), response.definitions);
                    for diagnostic in response.report.diagnostics {
                        let (uri, value) = diagnostic.into_lsp(document, uri, &mut files);
                        let diagnostics = result.entry(uri).or_default();
                        if !diagnostics.contains(&value) {
                            diagnostics.push(value);
                        }
                    }
                }
                Err(message) => {
                    result.entry(uri.clone()).or_default().push(json!({
                        "source": "marmot",
                        "severity": 1,
                        "range": CompilerLocation::default().range(&document.text),
                        "message": message
                    }));
                }
            }
        }
        (result, Definitions { entries, files })
    }

    fn analyze_entry(
        &self,
        entry: &Path,
        sources: &BTreeMap<String, &str>,
    ) -> Result<AnalysisResponse, String> {
        #[derive(Serialize)]
        struct AnalysisRequest<'a> {
            plan: plan::Plan,
            sources: &'a BTreeMap<String, &'a str>,
        }

        let (plan, warnings) = plan::analysis_plan(entry, &self.search_paths, &self.version)?;
        for warning in warnings {
            eprintln!("[packages] {warning}");
        }
        let mut child = Command::new(&self.executable)
            .arg("analyze")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|error| format!("Cannot start marmotc analyze: {error}"))?;
        let input = child.stdin.take().expect("piped child stdin");
        let written = serde_json::to_writer(input, &AnalysisRequest { plan, sources });
        let output = child
            .wait_with_output()
            .map_err(|error| format!("Cannot read marmotc analysis: {error}"))?;
        let response =
            serde_json::from_slice::<AnalysisResponse>(&output.stdout).map_err(|error| {
                let stderr = String::from_utf8_lossy(&output.stderr);
                let detail = stderr.trim();
                if detail.is_empty() {
                    format!("Invalid response from marmotc analyze: {error}")
                } else {
                    format!("marmotc analyze failed: {detail}")
                }
            })?;
        written.map_err(|error| format!("Cannot send analysis request: {error}"))?;
        Ok(response)
    }
}

struct Source {
    uri: String,
    text: Arc<str>,
}

fn source_for<'a>(files: &'a mut HashMap<String, Source>, file: &Path) -> Option<&'a Source> {
    let uri = Url::from_file_path(file).ok()?;
    let key = paths::identity_key(file);
    Some(files.entry(key).or_insert_with(|| Source {
        uri: uri.into(),
        text: std::fs::read_to_string(file).unwrap_or_default().into(),
    }))
}

#[derive(Deserialize)]
struct AnalysisResponse {
    report: Report,
    #[serde(default)]
    definitions: Vec<CompilerDefinition>,
}

#[derive(Deserialize)]
struct CompilerDefinition {
    reference: CompilerSpan,
    declaration: CompilerSpan,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CompilerSpan {
    file: PathBuf,
    line: u32,
    column: usize,
    end_line: u32,
    end_column: usize,
}

#[derive(Deserialize)]
struct Report {
    diagnostics: Vec<CompilerDiagnostic>,
}

#[derive(Default, Deserialize)]
#[serde(rename_all = "camelCase")]
struct CompilerLocation {
    file: Option<PathBuf>,
    line: Option<u32>,
    column: Option<usize>,
    end_line: Option<u32>,
    end_column: Option<usize>,
}

impl CompilerLocation {
    fn range(&self, text: &str) -> Value {
        let line = self.line.unwrap_or(1).saturating_sub(1);
        let end_line = self
            .end_line
            .map_or(line, |end| end.saturating_sub(1))
            .max(line);
        let column = self.column.unwrap_or(0);
        let end_column = self.end_column.unwrap_or_else(|| {
            if end_line == line && self.column.is_some() {
                column + 1
            } else {
                0
            }
        });
        let character = utf16_column(text, line, column);
        let end_character = utf16_column(text, end_line, end_column);
        json!({
            "start": { "line": line, "character": character },
            "end": { "line": end_line, "character": if end_line == line { end_character.max(character) } else { end_character } }
        })
    }
}

fn utf16_column(text: &str, line: u32, byte: usize) -> usize {
    let line = text.lines().nth(line as usize).unwrap_or("");
    let mut byte = byte.min(line.len());
    while !line.is_char_boundary(byte) {
        byte -= 1;
    }
    line[..byte].encode_utf16().count()
}

fn byte_column(text: &str, line: u32, character: usize) -> Option<usize> {
    let line = text.lines().nth(line as usize)?;
    let mut utf16 = 0;
    for (byte, ch) in line.char_indices() {
        if utf16 == character {
            return Some(byte);
        }
        utf16 += ch.len_utf16();
    }
    (utf16 == character).then_some(line.len())
}

#[derive(Deserialize)]
struct RelatedInformation {
    #[serde(flatten)]
    location: CompilerLocation,
    message: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CompilerDiagnostic {
    #[serde(flatten)]
    location: CompilerLocation,
    severity: String,
    code: String,
    source: String,
    message: String,
    suggestion: Option<String>,
    related_information: Vec<RelatedInformation>,
}

impl CompilerDiagnostic {
    fn into_lsp(
        self,
        entry: &Document,
        entry_uri: &str,
        files: &mut HashMap<String, Source>,
    ) -> (String, Value) {
        let (uri, range) = self
            .location
            .file
            .as_deref()
            .and_then(|file| source_for(files, file))
            .map(|source| (source.uri.clone(), self.location.range(&source.text)))
            .unwrap_or_else(|| (entry_uri.to_string(), self.location.range(&entry.text)));
        let mut diagnostic = json!({
            "range": range,
            "severity": if self.severity == "warning" { 2 } else { 1 },
            "code": self.code,
            "source": self.source,
            "message": self.message
        });
        if let Some(suggestion) = self.suggestion {
            diagnostic["data"] = json!({ "suggestion": suggestion });
        }
        if !self.related_information.is_empty() {
            let locations: Vec<_> = self.related_information.into_iter().filter_map(|related| {
                let source = source_for(files, related.location.file.as_deref()?)?;
                Some(json!({
                    "location": { "uri": source.uri, "range": related.location.range(&source.text) },
                    "message": related.message
                }))
            }).collect();
            diagnostic["relatedInformation"] = json!(locations);
        }
        (uri, diagnostic)
    }
}
