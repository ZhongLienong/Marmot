const vscode = require("vscode");
const childProcess = require("child_process");
const path = require("path");

function pathKey(file) {
    const normalized = path.normalize(file);
    return process.platform === "win32" ? normalized.toLowerCase() : normalized;
}

function diagnosticPosition(line, column, document) {
    if (!document) {
        return new vscode.Position(line, column);
    }

    // Marmot reports UTF-8 byte offsets. VS Code positions count UTF-16 code units.
    const position = document.validatePosition(new vscode.Position(line, 0));
    const bytes = Buffer.from(document.lineAt(position.line).text);
    const character = bytes.subarray(0, column).toString().length;
    return new vscode.Position(position.line, character);
}

function diagnosticRange(item, document) {
    const startLine = (item.line ?? 1) - 1;
    const endLine = (item.endLine ?? item.line ?? 1) - 1;
    const startColumn = item.column ?? 0;
    const endColumn = item.endColumn ?? (endLine === startLine ? startColumn + 1 : 0);
    return new vscode.Range(
        diagnosticPosition(startLine, startColumn, document),
        diagnosticPosition(endLine, endColumn, document)
    );
}

function readDiagnostics(stdout) {
    try {
        const payload = JSON.parse(stdout);
        return payload?.report?.diagnostics;
    } catch {
        return undefined;
    }
}

function toDiagnostic(item, document, cwd, openFiles) {
    const file = item.file ? path.resolve(cwd, item.file) : document.fileName;
    const inDocument = pathKey(file) === pathKey(document.fileName);
    const range = inDocument ? diagnosticRange(item, document) : new vscode.Range(0, 0, 0, 1);
    const message = inDocument ? item.message : path.basename(file) + ": " + item.message;
    const diagnostic = new vscode.Diagnostic(
        range,
        message,
        item.severity === "warning" ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error
    );
    diagnostic.source = item.source;
    diagnostic.code = item.code;
    diagnostic.marmotSuggestion = item.suggestion;
    diagnostic.relatedInformation = [];

    function addRelated(info, relatedFile) {
        const location = new vscode.Location(
            vscode.Uri.file(relatedFile),
            diagnosticRange(info, openFiles.get(pathKey(relatedFile)))
        );
        diagnostic.relatedInformation.push(new vscode.DiagnosticRelatedInformation(location, info.message));
    }

    if (!inDocument) {
        addRelated(item, file);
    }
    for (const info of item.relatedInformation) {
        if (info.file) {
            addRelated(info, path.resolve(cwd, info.file));
        }
    }
    return diagnostic;
}

class SuggestionCodeActionProvider {
    provideCodeActions(_document, _range, context) {
        return context.diagnostics
            .filter((diagnostic) => typeof diagnostic.marmotSuggestion === "string")
            .map((diagnostic) => {
                const action = new vscode.CodeAction(
                    diagnostic.marmotSuggestion,
                    vscode.CodeActionKind.QuickFix
                );
                action.command = {
                    command: "marmot.showSuggestion",
                    title: diagnostic.marmotSuggestion,
                    arguments: [diagnostic.marmotSuggestion]
                };
                action.diagnostics = [diagnostic];
                return action;
            });
    }
}

function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection("marmot");
    const output = vscode.window.createOutputChannel("Marmot");
    const pending = new Map();
    const reportedFailures = new Set();

    function cancelCheck(key) {
        const check = pending.get(key);
        pending.delete(key);
        check?.abort();
    }

    function refreshDiagnostics(document) {
        if (!vscode.workspace.isTrusted || document.languageId !== "marmot" || document.uri.scheme !== "file") {
            return;
        }

        const key = document.uri.toString();
        cancelCheck(key);
        if (document.isDirty) {
            return;
        }

        const config = vscode.workspace.getConfiguration("marmot", document.uri);
        const executable = config.get("executablePath") || "marmot";
        const cwd = path.dirname(document.fileName);
        const version = document.version;
        const check = new AbortController();
        pending.set(key, check);

        childProcess.execFile(
            executable,
            ["check", document.fileName, "--format", "json"],
            { cwd, signal: check.signal },
            (error, stdout, stderr) => {
                if (pending.get(key) !== check) {
                    return;
                }
                pending.delete(key);
                if (document.version !== version || document.isClosed) {
                    return;
                }

                // Compiler errors return a report even when the exit status is nonzero.
                const diagnostics = readDiagnostics(stdout);
                if (!Array.isArray(diagnostics)) {
                    collection.delete(document.uri);
                    output.appendLine("Marmot check failed for " + document.fileName);
                    output.appendLine(stderr.trim() || error?.message || "Expected a JSON diagnostic report.");
                    if (!reportedFailures.has(executable)) {
                        reportedFailures.add(executable);
                        const message = error?.code === "ENOENT"
                            ? "Marmot could not find the project tool. Set marmot.executablePath or add marmot to PATH."
                            : "Marmot could not check the file.";
                        vscode.window.showWarningMessage(message, "Show Output").then((choice) => {
                            if (choice === "Show Output") {
                                output.show();
                            }
                        });
                    }
                    return;
                }

                const openFiles = new Map(vscode.workspace.textDocuments
                    .filter((open) => open.uri.scheme === "file" && !open.isDirty)
                    .map((open) => [pathKey(open.fileName), open]));
                collection.set(document.uri, diagnostics.map((item) => toDiagnostic(item, document, cwd, openFiles)));
            }
        );
    }

    context.subscriptions.push(
        collection,
        output,
        {
            dispose() {
                for (const key of pending.keys()) {
                    cancelCheck(key);
                }
            }
        },
        vscode.workspace.onDidOpenTextDocument(refreshDiagnostics),
        vscode.workspace.onDidSaveTextDocument(refreshDiagnostics),
        vscode.workspace.onDidCloseTextDocument((document) => {
            cancelCheck(document.uri.toString());
            collection.delete(document.uri);
        }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => {
            vscode.workspace.textDocuments.forEach(refreshDiagnostics);
        }),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration("marmot.executablePath")) {
                reportedFailures.clear();
                vscode.workspace.textDocuments
                    .filter((document) => event.affectsConfiguration("marmot.executablePath", document.uri))
                    .forEach(refreshDiagnostics);
            }
        }),
        vscode.languages.registerCodeActionsProvider(
            { language: "marmot", scheme: "file" },
            new SuggestionCodeActionProvider(),
            { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] }
        ),
        vscode.commands.registerCommand("marmot.showSuggestion", (suggestion) => {
            vscode.window.showInformationMessage(suggestion);
        })
    );

    vscode.workspace.textDocuments.forEach(refreshDiagnostics);
}

module.exports = { activate };
