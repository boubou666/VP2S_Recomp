// Decompiles the function containing an address, plus all of its direct callers.
// Args: <output file> <addr>
// @category VP2S

import java.io.PrintWriter;
import java.util.*;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Callers extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Function target = getFunctionContaining(toAddr(args[1]));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Function> funcs = new LinkedHashSet<>();
        funcs.add(target);
        for (Reference r : getReferencesTo(target.getEntryPoint())) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) funcs.add(f);
        }
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (Function f : funcs) {
                DecompileResults res = di.decompileFunction(f, 120, monitor);
                out.println("// ===== " + f.getEntryPoint() + " " + f.getName()
                    + (f == target ? " (target)" : " (caller)"));
                out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed");
            }
        }
        di.dispose();
        println("functions written: " + funcs.size());
    }
}
