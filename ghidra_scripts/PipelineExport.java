// Runs PS2Recomp's ExportPS2Functions on the current program, deriving the output
// names from the program: <outdir>/<name>.toml and <outdir>/<name>_functions.csv
// (a trailing ".elf" is dropped). Lets one headless session export a whole batch.
// Args: <outdir>
// @category VP2S

import java.io.File;
import ghidra.app.script.GhidraScript;

public class PipelineExport extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        String base = currentProgram.getName().replaceFirst("\\.elf$", "");
        File toml = new File(outDir, base + ".toml");
        File csv = new File(outDir, base + "_functions.csv");
        runScript("ExportPS2Functions.java", new String[] { toml.getPath(), csv.getPath() });
        println("pipeline export: " + base + " -> " + toml.getPath());
    }
}
