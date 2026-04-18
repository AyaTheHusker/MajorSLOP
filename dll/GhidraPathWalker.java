// Ghidra headless script: decompile MegaMUD's path step/walk functions
// to understand how it handles sneak→move→BS without bless firing
// @category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.FileWriter;
import java.io.PrintWriter;

public class GhidraPathWalker extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outpath = "/home/bucka/AI/mudproxy/dll/ghidra_pathwalk_output.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(outpath));

        pw.println("================================================================================");
        pw.println("PATH WALKER / BLESS SUPPRESS RE");
        pw.println("How does MegaMUD prevent bless during sneak→move→BS?");
        pw.println("================================================================================");
        pw.println();

        // Key functions to decompile:
        // FUN_00404ed0 - STEP_ADVANCEMENT (last in automation chain, handles path stepping)
        // FUN_00404320 - PRE_MOVE (clears IS_SNEAKING etc before direction)
        // FUN_0040b070 - COMBAT_EARLY (sneak skip logic)
        // FUN_00406b40 - AUTOMATION_DISPATCH (already have but need to check activity gates)
        // FUN_0040bad0 - COMBAT_HANDLER (already have partial, need BS initiation)
        // FUN_004519a0 - called when BUSY_LOCK set, what does it do?
        // FUN_0040d7e0 - called early in automation dispatch, entity state update?
        // FUN_00479dc0 - activity state setter (called with activity codes)
        // FUN_00479980 - another activity function
        //
        // Also: what sets/clears 0x5664 (param_1[0x1599])?
        // And: what sets ACTIVITY_STATE during path walking?

        long[] addrs = {
            0x00404ed0L,  // STEP_ADVANCEMENT - path step handler
            0x00404320L,  // PRE_MOVE - clears flags before direction
            0x0040b070L,  // COMBAT_EARLY - sneak skip
            0x004519a0L,  // Called when BUSY_LOCK set in automation dispatch
            0x0040d7e0L,  // Called early in automation dispatch
            0x00479dc0L,  // Activity state setter
            0x00479980L,  // Activity function
            0x00403950L,  // Called in automation dispatch else-branch
        };

        String[] names = {
            "STEP_ADVANCEMENT",
            "PRE_MOVE",
            "COMBAT_EARLY",
            "BUSY_LOCK_HANDLER (FUN_004519a0)",
            "ENTITY_UPDATE (FUN_0040d7e0)",
            "ACTIVITY_SETTER (FUN_00479dc0)",
            "ACTIVITY_FUNC (FUN_00479980)",
            "FUN_00403950",
        };

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        for (int i = 0; i < addrs.length; i++) {
            Address addr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(addrs[i]);
            Function func = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (func == null) func = getFunctionAt(addr);
            if (func == null) {
                pw.printf("=== NO FUNCTION AT 0x%08X (%s) ===\n\n", addrs[i], names[i]);
                continue;
            }

            pw.println("================================================================================");
            pw.printf("=== %s (VA 0x%08X) ===\n", names[i], addrs[i]);
            pw.printf("=== Size: %d bytes ===\n", func.getBody().getNumAddresses());
            pw.println("================================================================================");
            pw.println();

            DecompileResults result = decomp.decompileFunction(func, 300, monitor);
            if (result != null && result.decompileCompleted()) {
                String code = result.getDecompiledFunction().getC();
                pw.println(code);

                // Flag interesting offsets
                if (code.contains("0x1599") || code.contains("0x5664"))
                    pw.println("*** REFERENCES 0x5664 (param_1[0x1599]) ***");
                if (code.contains("0x1530") || code.contains("0x54c0") || code.contains("0x4cc0"))
                    pw.println("*** REFERENCES ACTIVITY_STATE ***");
                if (code.contains("0x15a2") || code.contains("0x5688"))
                    pw.println("*** REFERENCES IS_SNEAKING ***");
                if (code.contains("0x133f") || code.contains("0x4cfc"))
                    pw.println("*** REFERENCES BUSY_LOCK ***");
            } else {
                pw.println("DECOMPILATION FAILED");
                if (result != null) pw.println("Error: " + result.getErrorMessage());
            }
            pw.println();
        }

        decomp.dispose();
        pw.close();
        println("Output written to " + outpath);
    }
}
