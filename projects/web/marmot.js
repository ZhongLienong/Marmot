(function () {
    "use strict";

    const adapterUrl = new URL(document.currentScript.src);

    function plainResult(result) {
        return {
            ...result,
            output: result.output.replace(/\x1b\[[0-9;]*m/g, ""),
            error: result.error.replace(/\x1b\[[0-9;]*m/g, ""),
        };
    }

    function resourceUrl(name, options) {
        const located = options.locateFile ? options.locateFile(name, new URL(".", adapterUrl).href) : name;
        const url = new URL(located, adapterUrl);
        if (!url.search) {
            url.search = adapterUrl.search;
        }
        return url.href;
    }

    function loadFactory(name, factory, options) {
        return new Promise((resolve, reject) => {
            const script = document.createElement("script");
            script.src = resourceUrl(name, options);
            script.onload = () => {
                const createModule = globalThis[factory];
                script.remove();
                resolve(createModule);
            };
            script.onerror = () => {
                script.remove();
                reject(new Error(`Failed to load ${name}`));
            };
            document.head.appendChild(script);
        });
    }

    globalThis.createMarmotModule = async function (options = {}) {
        const [createCompiler, createVm] = await Promise.all([
            loadFactory("marmotc.js", "createMarmotcModule", options),
            loadFactory("marmotvm.js", "createMarmotvmModule", options),
        ]);
        const settings = { ...options, locateFile: name => resourceUrl(name, options) };
        const [compiler, vm] = await Promise.all([createCompiler({ ...settings }), createVm({ ...settings })]);

        return {
            FS: compiler.FS,
            FS_createPath(...args) {
                compiler.FS_createPath(...args);
                vm.FS_createPath(...args);
            },
            FS_createDataFile(...args) {
                compiler.FS_createDataFile(...args);
                vm.FS_createDataFile(...args);
            },
            compileMarmotCode: source => plainResult(compiler.compileMarmotCode(source)),
            runMarmotBytecode: bytes => plainResult(vm.runMarmotBytecode(bytes)),
            executeMarmotCode(source) {
                const compiled = plainResult(compiler.compileMarmotCode(source));
                if (!compiled.success) {
                    return compiled;
                }
                const executed = plainResult(vm.runMarmotBytecode(compiled.bytes));
                return {
                    ...executed,
                    output: compiled.output + executed.output,
                    compileReport: compiled.report,
                };
            },
        };
    };
})();
