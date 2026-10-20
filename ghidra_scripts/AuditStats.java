// Prints audit statistics for the loaded program: functions, instruction mix,
// VU0 macro-mode / MMI usage, and the biggest functions.
// @category VP2S

import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class AuditStats extends GhidraScript {

    private static final Set<String> MMI_PREFIXES = Set.of(
        "padd", "psub", "pext", "ppac", "pcpy", "pmadd", "pmsub", "pmult", "pdiv",
        "pmfh", "pmth", "pmax", "pmin", "pceq", "pcgt", "pand", "por", "pxor", "pnor",
        "psll", "psrl", "psra", "pabs", "pinth", "pintel", "pexe", "prev", "prot", "plzcw",
        "qfsrv", "mfhi1", "mflo1", "mthi1", "mtlo1", "mult1", "multu1", "div1", "divu1", "madd1");

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("Language: " + currentProgram.getLanguageID());
        println("Functions: " + fm.getFunctionCount());

        Map<String, Integer> mnem = new HashMap<>();
        long total = 0, vu0 = 0, mmi = 0, quad = 0, cop1 = 0;
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            String m = it.next().getMnemonicString().toLowerCase().replaceFirst("^_", "");
            mnem.merge(m, 1, Integer::sum);
            total++;
            if (m.startsWith("v") || m.equals("qmfc2") || m.equals("qmtc2") || m.equals("lqc2")
                    || m.equals("sqc2") || m.equals("cfc2") || m.equals("ctc2")) {
                vu0++;
            } else if (m.equals("lq") || m.equals("sq")) {
                quad++;
            } else if (m.endsWith(".s") || m.equals("lwc1") || m.equals("swc1")) {
                cop1++;
            } else {
                for (String p : MMI_PREFIXES) {
                    if (m.startsWith(p)) { mmi++; break; }
                }
            }
        }
        println(String.format("Instructions: %d | VU0 macro: %d | MMI: %d | lq/sq: %d | FPU: %d",
            total, vu0, mmi, quad, cop1));

        List<Map.Entry<String, Integer>> top = new ArrayList<>(mnem.entrySet());
        top.sort((a, b) -> b.getValue() - a.getValue());
        StringBuilder sb = new StringBuilder("Top mnemonics:");
        for (int i = 0; i < Math.min(40, top.size()); i++) {
            sb.append(' ').append(top.get(i).getKey()).append('=').append(top.get(i).getValue());
        }
        println(sb.toString());

        List<Function> funcs = new ArrayList<>();
        fm.getFunctions(true).forEach(funcs::add);
        funcs.sort((a, b) -> Long.compare(b.getBody().getNumAddresses(), a.getBody().getNumAddresses()));
        println("Largest functions:");
        for (int i = 0; i < Math.min(15, funcs.size()); i++) {
            Function f = funcs.get(i);
            println(String.format("  %s %s %d bytes", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses()));
        }
    }
}
