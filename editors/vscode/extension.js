const vscode = require("vscode");
const childProcess = require("child_process");
const fs = require("fs/promises");
const os = require("os");
const path = require("path");
const { promisify } = require("util");

const execFile = promisify(childProcess.execFile);

function executableFor(resource) {
    return vscode.workspace.getConfiguration("marmot", resource).get("executablePath") || "marmot";
}

async function formatText(executable, cwd, text) {
    // The formatter takes a file. Stage the editor buffer so it also sees unsaved edits.
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "marmot-format-"));
    const input = path.join(directory, "buffer.mmt");
    try {
        await fs.writeFile(input, text, "utf8");
        const { stdout } = await execFile(executable, ["fmt", input, "--format", "json"], { cwd, maxBuffer: Infinity });
        const report = JSON.parse(stdout);
        if (typeof report?.formattedText !== "string") {
            throw new Error("Expected formattedText in the Marmot formatter response.");
        }
        return report.formattedText;
    } finally {
        await fs.rm(input, { force: true });
        await fs.rmdir(directory);
    }
}

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

function runMarmot(resource, cwd, folder, file) {
    const args = file ? ["run", file] : ["run"];
    const task = new vscode.Task(
        { type: "marmot", target: file ?? cwd },
        folder ?? vscode.TaskScope.Global,
        file ? "Run " + path.basename(file) : "Run Project",
        "Marmot",
        new vscode.ProcessExecution(executableFor(resource), args, { cwd })
    );
    task.presentationOptions = { focus: true, reveal: vscode.TaskRevealKind.Always };
    return vscode.tasks.executeTask(task);
}

async function runFile(resource) {
    if (!vscode.workspace.isTrusted) {
        return;
    }

    const document = resource instanceof vscode.Uri
        ? await vscode.workspace.openTextDocument(resource)
        : vscode.window.activeTextEditor?.document;
    if (document?.languageId !== "marmot" || document.uri.scheme !== "file") {
        vscode.window.showWarningMessage("Open a saved Marmot file to run it.");
        return;
    }
    if (document.isDirty && !(await document.save())) {
        return;
    }

    const file = document.uri.fsPath;
    const folder = vscode.workspace.getWorkspaceFolder(document.uri);
    return runMarmot(document.uri, path.dirname(file), folder, file);
}

async function runProject() {
    if (!vscode.workspace.isTrusted) {
        return;
    }

    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
        vscode.window.showWarningMessage("Open a Marmot project folder to run it.");
        return;
    }

    const resource = vscode.window.activeTextEditor?.document.uri;
    const activeFolder = resource && vscode.workspace.getWorkspaceFolder(resource);
    const folder = activeFolder ?? (folders.length === 1
        ? folders[0]
        : await vscode.window.showWorkspaceFolderPick({ placeHolder: "Select a Marmot project folder" }));
    if (!folder) {
        return;
    }
    if (folder.uri.scheme !== "file") {
        vscode.window.showWarningMessage("Open a local Marmot project folder to run it.");
        return;
    }

    return runMarmot(folder.uri, folder.uri.fsPath, folder);
}

function activate(context) {
    const collection = vscode.languages.createDiagnosticCollection("marmot");
    const output = vscode.window.createOutputChannel("Marmot");
    const pending = new Map();
    const reportedFailures = new Set();

    function reportToolFailure(executable, error, fallback) {
        if (reportedFailures.has(executable)) {
            return;
        }
        reportedFailures.add(executable);
        const message = error?.code === "ENOENT"
            ? "Marmot could not find the project tool. Set marmot.executablePath or add marmot to PATH."
            : fallback;
        vscode.window.showWarningMessage(message, "Show Output").then((choice) => {
            if (choice === "Show Output") {
                output.show();
            }
        });
    }

    async function provideDocumentFormattingEdits(document, _options, token) {
        const version = document.version;
        const isCurrent = () => !token.isCancellationRequested && !document.isClosed && document.version === version;
        if (!vscode.workspace.isTrusted || !isCurrent()) {
            return [];
        }

        const executable = executableFor(document.uri);
        const cwd = document.uri.scheme === "file" ? path.dirname(document.fileName) : undefined;
        const text = document.getText();
        let formatted;
        try {
            formatted = await formatText(executable, cwd, text);
        } catch (error) {
            if (isCurrent()) {
                output.appendLine("Marmot format failed for " + document.fileName);
                output.appendLine(error.stderr?.trim() || error.message);
                reportToolFailure(executable, error, "Marmot could not format the document.");
            }
            return [];
        }
        if (!isCurrent()) {
            return [];
        }

        const eol = document.eol === vscode.EndOfLine.CRLF ? "\r\n" : "\n";
        formatted = formatted.replace(/\r?\n/g, eol);
        if (formatted === text) {
            return [];
        }

        const range = new vscode.Range(document.positionAt(0), document.positionAt(text.length));
        return [vscode.TextEdit.replace(range, formatted)];
    }

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

        const executable = executableFor(document.uri);
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
                    reportToolFailure(executable, error, "Marmot could not check the file.");
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
        vscode.languages.registerDocumentFormattingEditProvider(
            [{ language: "marmot", scheme: "file" }, { language: "marmot", scheme: "untitled" }],
            { provideDocumentFormattingEdits }
        ),
        vscode.commands.registerCommand("marmot.runFile", runFile),
        vscode.commands.registerCommand("marmot.runProject", runProject),
        vscode.commands.registerCommand("marmot.showSuggestion", (suggestion) => {
            vscode.window.showInformationMessage(suggestion);
        })
    );

    vscode.workspace.textDocuments.forEach(refreshDiagnostics);
}

module.exports = { activate };
