importScripts(`/marmot.js${self.location.search}`);

(async () => {
    const module = await createMarmotModule();
    module.FS_createPath("/", "frames", true, true);
    const result = module.executeMarmotCode(`
module WorkerSmoke
foreign "MARMOT_FFI_WriteBinaryFile" WriteBinaryFile: fn(Text, Array<Byte>) -> Bool from "marmotstdlib";
WriteBinaryFile("/frames/1.rgba", [1 as Byte, 2 as Byte, 3 as Byte, 255 as Byte]);
`);
    if (!result.success) {
        throw new Error(result.error);
    }
    if (!module.FS_readDir("/frames").includes("1.rgba") ||
        module.FS_readBinaryFile("/frames/1.rgba").join(",") !== "1,2,3,255") {
        throw new Error("VM file is missing or has the wrong bytes");
    }
    module.FS_unlink("/frames/1.rgba");
    if (module.FS_readDir("/frames").includes("1.rgba")) {
        throw new Error("VM file still exists after unlink");
    }
    self.postMessage("ok");
})().catch(error => self.postMessage(String(error)));
