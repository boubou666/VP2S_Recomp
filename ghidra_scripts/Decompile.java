// Decompiles the functions at the given addresses into an output file.
// Args: <output file> <addr> [addr...]
// @category VP2S

import java.io.PrintWriter;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Function f = getFunctionAt(toAddr(args[i]));
                if (f == null) f = getFunctionContaining(toAddr(args[i]));
                if (f == null) {
                    // Routines only reached through pointers may be missing: create them (not saved with -readOnly)
                    disassemble(toAddr(args[i]));
                    f = createFunction(toAddr(args[i]), null);
                }
                if (f == null) { out.println("// no function at " + args[i]); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                out.println("// ===== " + f.getEntryPoint() + " " + f.getName());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        di.dispose();
        println("wrote " + args[0]);
    }
}
