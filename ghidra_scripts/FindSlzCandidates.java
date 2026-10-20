// Ranks functions likely to be the SLZ decompressor: immediate 'S','L','Z' compares,
// and LZ-loop shape (byte loads/stores, shifts, small masks).
// @category VP2S

import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;

public class FindSlzCandidates extends GhidraScript {

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        List<String[]> rows = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            Set<Long> scalars = new HashSet<>();
            int lbu = 0, sb = 0, shifts = 0, masks = 0, total = 0, branchesBack = 0;
            for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                total++;
                String m = ins.getMnemonicString().toLowerCase().replaceFirst("^_", "");
                if (m.equals("lbu") || m.equals("lb")) lbu++;
                if (m.equals("sb")) sb++;
                if (m.startsWith("sll") || m.startsWith("srl") || m.startsWith("sra")) shifts++;
                for (int op = 0; op < ins.getNumOperands(); op++) {
                    Scalar s = ins.getScalar(op);
                    if (s == null) continue;
                    long v = s.getUnsignedValue();
                    scalars.add(v);
                    if (m.equals("andi") && (v == 0xf || v == 0xff || v == 0xfff || v == 0x3ff || v == 0x7ff || v == 0x1f)) masks++;
                }
                if (ins.getFlowType().isJump() && ins.getFlows().length > 0
                        && ins.getFlows()[0].compareTo(ins.getAddress()) < 0) branchesBack++;
            }
            boolean magic = scalars.contains(0x53L) && scalars.contains(0x4cL) && scalars.contains(0x5aL);
            int score = (magic ? 50 : 0) + Math.min(lbu, 20) + Math.min(sb, 20) + Math.min(shifts, 15)
                + masks * 3 + Math.min(branchesBack, 6) * 2;
            if (total < 20 || total > 2000) continue;
            rows.add(new String[]{String.format("%05d", score), f.getEntryPoint().toString(),
                String.format("score=%d magic=%s lbu=%d sb=%d shifts=%d masks=%d loops=%d size=%d",
                    score, magic, lbu, sb, shifts, masks, branchesBack, total)});
        }
        rows.sort((a, b) -> b[0].compareTo(a[0]));
        for (int i = 0; i < Math.min(15, rows.size()); i++) {
            println(rows.get(i)[1] + " " + rows.get(i)[2]);
        }
    }
}
