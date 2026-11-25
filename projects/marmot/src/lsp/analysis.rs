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

pub(super) struct Symbols {
    entries: BTreeMap<String, DocumentSymbols>,
    files: HashMap<String, Source>,
}

impl Symbols {
    pub(super) fn definition(
        &mut self,
        uri: &str,
        text: &str,
        line: u32,
        character: usize,
    ) -> Option<Value> {
        let position = (line + 1, byte_column(text, line, character)?);
        let declaration = &self.entries.get(uri)?.declaration(position)?.span;
        let source = source_for(&mut self.files, &declaration.file)?;
        Some(json!({ "uri": source.uri, "range": declaration.range(&source.text) }))
    }

    pub(super) fn hover(
        &mut self,
        uri: &str,
        text: &str,
        line: u32,
        character: usize,
    ) -> Option<Value> {
        let position = (line + 1, byte_column(text, line, character)?);
        let entry = self.entries.get(uri)?;
        let symbol = entry
            .types
            .iter()
            .find(|symbol| symbol.span.contains(position))?;
        let documentation = if let Some(line) = symbol.documentation_line {
            Some((&symbol.span, line))
        } else {
            entry
                .declaration(position)
                .and_then(|declaration| Some((&declaration.span, declaration.documentation_line?)))
        };
        let mut value = symbol.type_name.clone();
        if let Some((declaration, first_line)) = documentation
            && let Some(source) = source_for(&mut self.files, &declaration.file)
        {
            let comment = doc_comment(&source.text, first_line, declaration.line);
            if !comment.is_empty() {
                value.push_str("\n\n");
                value.push_str(&comment);
            }
        }
        Some(json!({
            "contents": { "kind": "plaintext", "value": value },
            "range": symbol.span.range(text)
        }))
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

    pub(super) fn analyze(&self, documents: &Documents) -> (Diagnostics, Symbols) {
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
                    entries.insert(uri.clone(), response.symbols);
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
        (result, Symbols { entries, files })
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

fn doc_comment(text: &str, first_line: u32, declaration_line: u32) -> String {
    text.lines()
        .skip(first_line as usize - 1)
        .take((declaration_line - first_line) as usize)
        .filter_map(|line| line.trim_start().strip_prefix("///"))
        .map(|comment| comment.strip_prefix(' ').unwrap_or(comment))
        .collect::<Vec<_>>()
        .join("\n")
}

#[derive(Deserialize)]
struct AnalysisResponse {
    report: Report,
    #[serde(flatten)]
    symbols: DocumentSymbols,
}

#[derive(Deserialize)]
struct DocumentSymbols {
    #[serde(default)]
    definitions: Vec<CompilerDefinition>,
    #[serde(default)]
    types: Vec<CompilerType>,
}

impl DocumentSymbols {
    fn declaration(&self, position: (u32, usize)) -> Option<&CompilerDeclaration> {
        self.definitions
            .iter()
            .find(|definition| definition.reference.contains(position))
            .map(|definition| &definition.declaration)
    }
}

#[derive(Deserialize)]
struct CompilerDefinition {
    reference: CompilerSpan,
    declaration: CompilerDeclaration,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CompilerDeclaration {
    #[serde(flatten)]
    span: CompilerSpan,
    documentation_line: Option<u32>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CompilerType {
    span: CompilerSpan,
    #[serde(rename = "type")]
    type_name: String,
    documentation_line: Option<u32>,
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

impl CompilerSpan {
    fn contains(&self, position: (u32, usize)) -> bool {
        (self.line, self.column) <= position && position < (self.end_line, self.end_column)
    }

    fn range(&self, text: &str) -> Value {
        CompilerLocation {
            line: Some(self.line),
            column: Some(self.column),
            end_line: Some(self.end_line),
            end_column: Some(self.end_column),
            ..CompilerLocation::default()
        }
        .range(text)
    }
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
