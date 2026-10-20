// Locates functions referencing key strings (file access) and the 'SLZ' magic constant.
// @category VP2S

import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;

public class FindKeyFunctions extends GhidraScript {

    private static final String[] STRINGS = {"pfs0:vp2.bin", "cdrom0:\\IOPRP300.IMG;1", "hdd0:PP.SLES-54645", "pfs0:/res"};
    // 'SLZ' as little-endian u32 prefix (0x005A4C53) and its upper half used by lui
    private static final long[] CONSTS = {0x5A4C53L, 0x5A4CL, 0x4C53L};

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        for (String s : STRINGS) {
            Address a = find(null, s.getBytes("ASCII"));
            if (a == null) { println("string not found: " + s); continue; }
            Set<String> users = new TreeSet<>();
            for (Reference r : getReferencesTo(a)) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                users.add(f == null ? r.getFromAddress().toString() : f.getEntryPoint() + " " + f.getName());
            }
            println(String.format("'%s' @ %s used by %s", s, a, users));
        }

        Map<Long, Set<String>> hits = new HashMap<>();
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                Scalar sc = ins.getScalar(op);
                if (sc == null) continue;
                long v = sc.getUnsignedValue();
                for (long c : CONSTS) {
                    if (v == c) {
                        Function f = fm.getFunctionContaining(ins.getAddress());
                        hits.computeIfAbsent(c, k -> new TreeSet<>())
                            .add((f == null ? "?" : f.getEntryPoint().toString()) + ":" + ins.getAddress() + " " + ins);
                    }
                }
            }
        }
        for (Map.Entry<Long, Set<String>> e : hits.entrySet()) {
            println(String.format("const 0x%X: %s", e.getKey(), e.getValue()));
        }
        if (hits.isEmpty()) println("no SLZ constant found in main ELF code");
    }
}
