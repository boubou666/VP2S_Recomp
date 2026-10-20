// Lists every instruction using one of the given immediate values, grouped by function.
// Args: <hex value> [hex value...]
// @category VP2S

import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;

public class FindScalar extends GhidraScript {

    @Override
    public void run() throws Exception {
        Set<Long> wanted = new HashSet<>();
        for (String a : getScriptArgs()) wanted.add(Long.parseLong(a.replaceFirst("^0x", ""), 16));
        FunctionManager fm = currentProgram.getFunctionManager();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                Scalar s = ins.getScalar(op);
                // MIPS immediates are 16-bit and often sign-extended: also match on the low 16 bits
                if (s != null && (wanted.contains(s.getUnsignedValue()) || wanted.contains(s.getValue())
                        || wanted.contains(s.getValue() & 0xffffL))) {
                    Function f = fm.getFunctionContaining(ins.getAddress());
                    println(String.format("%s %s : %s", f == null ? "?" : f.getEntryPoint(), ins.getAddress(), ins));
                }
            }
        }
    }
}
