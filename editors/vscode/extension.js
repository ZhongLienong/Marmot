const vscode = require("vscode");
const childProcess = require("child_process");
const fs = require("fs/promises");
const os = require("os");
const path = require("path");
const { promisify } = require("util");
const { LanguageClient } = require("vscode-languageclient/node");

const execFile = promisify(childProcess.execFile);
const marmotFile = { language: "marmot", scheme: "file" };
let stopLanguageClients;

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
    const output = vscode.window.createOutputChannel("Marmot");
    const clients = new Map();
    let clientUpdates = Promise.resolve();
    let nextClientId = 0;
    let stopping = false;
    const reportedFailures = new Set();

    function logError(error) {
        output.appendLine(String(error));
    }

    async function disposeClients() {
        const active = [...clients.values()];
        clients.clear();
        await Promise.all(active.map(async ({ client, started }) => {
            await started;
            await client.dispose().catch(logError);
        }));
    }

    function updateLanguageClients(restart = false) {
        clientUpdates = clientUpdates.then(async () => {
            if (restart) {
                await disposeClients();
            }
            if (stopping || !vscode.workspace.isTrusted) {
                return;
            }

            const executables = new Set(vscode.workspace.textDocuments
                .filter((document) => vscode.languages.match(marmotFile, document) > 0)
                .map((document) => executableFor(document.uri)));
            for (const executable of executables) {
                if (clients.has(executable)) {
                    continue;
                }
                // Files using the same toolchain share their unsaved import buffers.
                const belongsToClient = (document) => executableFor(document.uri) === executable;
                const sendDocument = async (document, next) => {
                    if (belongsToClient(document)) {
                        await next(document);
                    }
                };
                const client = new LanguageClient(
                    "marmot-" + nextClientId++,
                    "Marmot",
                    // The client's default is stdio. An explicit transport appends a flag.
                    { command: executable, args: ["lsp"] },
                    {
                        documentSelector: [marmotFile],
                        outputChannel: output,
                        middleware: {
                            didOpen: sendDocument,
                            didChange: async (event, next) => {
                                if (belongsToClient(event.document)) {
                                    await next(event);
                                }
                            },
                            didSave: sendDocument,
                            didClose: sendDocument
                        }
                    }
                );
                clients.set(executable, { client, started: client.start().catch(logError) });
            }
        }).catch(logError);
    }

    stopLanguageClients = () => {
        stopping = true;
        return clientUpdates.then(disposeClients);
    };

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

    context.subscriptions.push(
        output,
        vscode.workspace.onDidOpenTextDocument((document) => {
            if (vscode.languages.match(marmotFile, document) > 0) {
                updateLanguageClients();
            }
        }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => updateLanguageClients()),
        vscode.workspace.onDidChangeWorkspaceFolders(() => updateLanguageClients(true)),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration("marmot.executablePath")) {
                reportedFailures.clear();
                updateLanguageClients(true);
            }
        }),
        vscode.languages.registerCodeActionsProvider(
            marmotFile,
            {
                provideCodeActions(document, _range, context) {
                    const client = clients.get(executableFor(document.uri))?.client;
                    if (!client) {
                        return [];
                    }
                    return context.diagnostics.flatMap((diagnostic) => {
                        const suggestion = client.code2ProtocolConverter.asDiagnostic(diagnostic).data?.suggestion;
                        if (typeof suggestion !== "string") {
                            return [];
                        }
                        const action = new vscode.CodeAction(suggestion, vscode.CodeActionKind.QuickFix);
                        action.command = { command: "marmot.showSuggestion", title: suggestion, arguments: [suggestion] };
                        action.diagnostics = [diagnostic];
                        return action;
                    });
                }
            },
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

    updateLanguageClients();
}

function deactivate() {
    return stopLanguageClients?.();
}

module.exports = { activate, deactivate };
