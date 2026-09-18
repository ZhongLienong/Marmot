const vscode = require("vscode");
const childProcess = require("child_process");
const path = require("path");

function normalizePath(value) {
    return value.replace(/\\/g, "/").toLowerCase();
}

function diagnosticRange(document, item) {
    const startLine = Math.max((item.line ?? 1) - 1, 0);
    const startColumn = Math.max(item.column ?? 0, 0);
    const endLine = Math.max((item.endLine ?? item.line ?? 1) - 1, 0);
    const endColumn = Math.max(item.endColumn ?? startColumn + 1, startColumn + 1);
    return new vscode.Range(startLine, startColumn, endLine, endColumn);
}

function diagnosticSeverity(value) {
    switch (value) {
        case "warning":
            return vscode.DiagnosticSeverity.Warning;
        case "error":
        default:
            return vscode.DiagnosticSeverity.Error;
    }
}

function midoriExecutablePath() {
    const config = vscode.workspace.getConfiguration("marmot");
    return config.get("executablePath", process.platform === "win32" ? "Marmot.exe" : "marmot");
}

function refreshDiagnostics(document, collection) {
    if (document.languageId !== "marmot" || document.uri.scheme !== "file") {
        return;
    }

    const executable = midoriExecutablePath();
    childProcess.execFile(
        executable,
        ["check", document.fileName, "--format", "json"],
        { cwd: path.dirname(document.fileName) },
        (error, stdout, stderr) => {
            if (stderr && stderr.trim() !== "") {
                collection.set(document.uri, []);
                return;
            }

            let payload;
            try {
                payload = JSON.parse(stdout);
            } catch {
                collection.set(document.uri, []);
                return;
            }

            const report = payload.report ?? payload;
            const diagnostics = Array.isArray(report.diagnostics) ? report.diagnostics : [];
            const documentKey = normalizePath(document.fileName);
            const mapped = diagnostics
                .filter((item) => typeof item === "object" && item !== null)
                .filter((item) => {
                    const file = item.file ?? item.file_path;
                    return typeof file !== "string" || normalizePath(file) === documentKey;
                })
                .map((item) => {
                    const diagnostic = new vscode.Diagnostic(
                        diagnosticRange(document, item),
                        item.message ?? "Marmot diagnostic",
                        diagnosticSeverity(item.severity)
                    );
                    diagnostic.source = item.source ?? "marmot";
                    diagnostic.code = item.code ?? undefined;
                    diagnostic.relatedInformation = Array.isArray(item.relatedInformation)
                        ? item.relatedInformation
                            .filter((info) => info && typeof info.message === "string" && typeof info.file === "string")
                            .map((info) => new vscode.DiagnosticRelatedInformation(
                                new vscode.Location(
                                    vscode.Uri.file(info.file),
                                    new vscode.Range(
                                        Math.max((info.line ?? 1) - 1, 0),
                                        Math.max(info.column ?? 0, 0),
                                        Math.max((info.endLine ?? info.line ?? 1) - 1, 0),
                                        Math.max(info.endColumn ?? (info.column ?? 0) + 1, (info.column ?? 0) + 1)
                                    )
                                ),
                                info.message
                            ))
                        : [];
                    diagnostic.midoriSuggestion = typeof item.suggestion === "string" ? item.suggestion : undefined;
                    return diagnostic;
                });

            collection.set(document.uri, mapped);
        }
    );
}

class SuggestionCodeActionProvider {
    provideCodeActions(_document, _range, context) {
        return context.diagnostics
            .filter((diagnostic) => typeof diagnostic.midoriSuggestion === "string")
            .map((diagnostic) => {
                const action = new vscode.CodeAction(
                    diagnostic.midoriSuggestion,
                    vscode.CodeActionKind.QuickFix
                );
                action.command = {
                    command: "marmot.showSuggestion",
                    title: diagnostic.midoriSuggestion,
                    arguments: [diagnostic.midoriSuggestion]
                };
                action.diagnostics = [diagnostic];
                return action;
            });
    }
}

function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection("marmot");
    context.subscriptions.push(collection);

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument((document) => refreshDiagnostics(document, collection))
    );
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument((document) => refreshDiagnostics(document, collection))
    );
    context.subscriptions.push(
        vscode.workspace.onDidCloseTextDocument((document) => collection.delete(document.uri))
    );
    context.subscriptions.push(
        vscode.languages.registerCodeActionsProvider(
            { language: "marmot" },
            new SuggestionCodeActionProvider(),
            { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] }
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand("marmot.showSuggestion", (suggestion) => {
            if (typeof suggestion === "string") {
                vscode.window.showInformationMessage(suggestion);
            }
        })
    );

    for (const document of vscode.workspace.textDocuments) {
        refreshDiagnostics(document, collection);
    }
}

function deactivate() {}

module.exports = {
    activate,
    deactivate
};
